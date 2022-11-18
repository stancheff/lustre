// SPDX-License-Identifier: GPL-2.0
/*
 * This contains encryption functions for per-file encryption.
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility
 *
 * Written by Michael Halcrow, 2014.
 *
 * Filename encryption additions
 *	Uday Savagaonkar, 2014
 * Encryption policy handling additions
 *	Ildar Muslukhov, 2014
 * Add llcrypt_pullback_bio_page()
 *	Jaegeuk Kim, 2015.
 *
 * This has not yet undergone a rigorous security audit.
 *
 * The usage of AES-XTS should conform to recommendations in NIST
 * Special Publication 800-38E and IEEE P1619/D16.
 */
/*
 * Linux commit 219d54332a09
 * tags/v5.4
 */

#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/namei.h>
#include <lustre_compat.h>
#include "llcrypt_private.h"

static void __llcrypt_decrypt_bio(struct bio *bio, bool done)
{
	struct bio_vec *bv;
#ifdef HAVE_BVEC_ITER_ALL
	struct bvec_iter_all iter_all;
#else
	int iter_all;
#endif

	bio_for_each_segment_all(bv, bio, iter_all) {
		struct page *page = bv->bv_page;
		int ret = llcrypt_decrypt_pagecache_blocks(page, bv->bv_len,
							   bv->bv_offset);
		if (ret)
			SetPageError(page);
		else if (done)
			SetPageUptodate(page);
		if (done)
			unlock_page(page);
	}
}

void llcrypt_decrypt_bio(struct bio *bio)
{
	__llcrypt_decrypt_bio(bio, false);
}
EXPORT_SYMBOL(llcrypt_decrypt_bio);

static void completion_pages(struct work_struct *work)
{
	struct llcrypt_ctx *ctx = container_of(work, struct llcrypt_ctx, work);
	struct bio *bio = ctx->bio;

	__llcrypt_decrypt_bio(bio, true);
	llcrypt_release_ctx(ctx);
	bio_put(bio);
}

void llcrypt_enqueue_decrypt_bio(struct llcrypt_ctx *ctx, struct bio *bio)
{
	INIT_WORK(&ctx->work, completion_pages);
	ctx->bio = bio;
	llcrypt_enqueue_decrypt_work(&ctx->work);
}
EXPORT_SYMBOL(llcrypt_enqueue_decrypt_bio);

int llcrypt_zeroout_range(const struct inode *inode, pgoff_t lblk,
				sector_t pblk, unsigned int len)
{
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocksize = 1 << blockbits;
	struct page *ciphertext_page;
	struct bio *bio;
	int ret, err = 0;

	ciphertext_page = llcrypt_alloc_bounce_page(GFP_NOWAIT);
	if (!ciphertext_page)
		return -ENOMEM;

	while (len--) {
		err = llcrypt_crypt_block(inode, FS_ENCRYPT, lblk,
					  ZERO_PAGE(0), ciphertext_page,
					  blocksize, 0, GFP_NOFS);
		if (err)
			goto errout;

		bio = cfs_bio_alloc(inode->i_sb->s_bdev, 1, REQ_OP_WRITE,
				    GFP_NOWAIT);
		if (!bio) {
			err = -ENOMEM;
			goto errout;
		}
		bio->bi_iter.bi_sector = pblk << (blockbits - 9);
		ret = bio_add_page(bio, ciphertext_page, blocksize, 0);
		if (WARN_ON(ret != blocksize)) {
			/* should never happen! */
			bio_put(bio);
			err = -EIO;
			goto errout;
		}
		err = submit_bio_wait(bio);
		if (err == 0 && bio->bi_status)
			err = -EIO;
		bio_put(bio);
		if (err)
			goto errout;
		lblk++;
		pblk++;
	}
	err = 0;
errout:
	llcrypt_free_bounce_page(ciphertext_page);
	return err;
}
EXPORT_SYMBOL(llcrypt_zeroout_range);
