// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 NVIDIA Corporation
 *
 * File data verification using iomap infrastructure.
 *
 * Author: Chaitanya Kulkarni <kch@nvidia.com>
 *
 */
#include <linux/iomap.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>

#include "trace.h"

/**
 * iomap_file_verify - verify file data blocks are readable on storage media
 * @inode: inode to verify
 * @pos: starting byte position
 * @len: number of bytes to verify
 * @ops: iomap operations for extent mapping
 * @flags: FSVERIFY_RANGE_* flags from userspace
 *
 * Iterates over file extents using iomap and issues REQ_OP_VERIFY
 * requests to the underlying block device for each mapped extent.
 *
 * Extent types handled:
 * - IOMAP_MAPPED: issue verify to physical blocks
 * - IOMAP_HOLE: skip, nothing to verify
 * - IOMAP_UNWRITTEN: skip, no user data present
 * - IOMAP_INLINE: skip, data in inode not on block device
 * - IOMAP_DELALLOC: skip, not yet allocated
 *
 * Returns:
 *   0 on success
 *   -EOPNOTSUPP if hardware verify not supported and FSVERIFY_RANGE_NOFALLBACK
 *   -EIO on verification failure (media error detected)
 *   Other negative errno on failure
 */
int iomap_file_verify(struct inode *inode, loff_t pos, loff_t len,
		      const struct iomap_ops *ops, unsigned int flags)
{
	struct iomap_iter iter = {
		.inode	= inode,
		.pos	= pos,
		.len	= len,
		.flags	= IOMAP_REPORT,
	};
	unsigned int blkdev_flags = 0;
	int ret = 0;

	/* Map userspace flags to block layer flags */
	if (flags & FSVERIFY_RANGE_NOFALLBACK)
		blkdev_flags |= BLKDEV_VERIFY_NOFALLBACK;

	while ((ret = iomap_iter(&iter, ops)) > 0) {
		struct iomap *iomap = &iter.iomap;
		unsigned int lb_size;
		u64 extent_len;
		sector_t nr_sects;
		sector_t sector;
		int verify_ret;

		/* Check for fatal signals if killable */
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		/*
		 * Skip extent types that don't have data on block device:
		 * - HOLE: sparse region, nothing to verify
		 * - UNWRITTEN: preallocated but no user data
		 * - INLINE: data stored in inode itself
		 * - DELALLOC: delayed allocation, not on disk yet
		 */
		switch (iomap->type) {
		case IOMAP_HOLE:
		case IOMAP_UNWRITTEN:
		case IOMAP_INLINE:
		case IOMAP_DELALLOC:
			iter.status = iomap_iter_advance_full(&iter);
			continue;
		case IOMAP_MAPPED:
			if (!iomap->bdev) {
				iter.status = -EOPNOTSUPP;
				goto out;
			}
			break;
		default:
			/* Unknown extent type - should not happen */
			WARN_ON_ONCE(1);
			iter.status = iomap_iter_advance_full(&iter);
			continue;
		}

		/* Get the length for this portion of the extent */
		extent_len = iomap_length(&iter);

		/*
		 * blkdev_issue_verify() requires requests aligned to the
		 * device logical block size. The VFS layer (fs/ioctl.c)
		 * should ensure alignment before calling us, but check
		 * here as a safety measure.
		 */
		lb_size = bdev_logical_block_size(iomap->bdev);
		if ((iter.pos | extent_len) & (lb_size - 1)) {
			pr_warn("VER_DBG: %-30s ino=%lu MISALIGNED: pos=%lld len=%lld lb_size=%u\n",
				__func__, inode->i_ino, iter.pos, extent_len,
				lb_size);
			iter.status = -EINVAL;
			goto out;
		}

		sector = iomap_sector(iomap, iter.pos);
		nr_sects = extent_len >> SECTOR_SHIFT;

		/* Issue verify to block device */
		verify_ret = blkdev_issue_verify(iomap->bdev, sector, nr_sects,
						 GFP_KERNEL, blkdev_flags);
		trace_iomap_verify_extent(inode, iomap, iter.pos, sector,
					  nr_sects, verify_ret);
		if (verify_ret) {
			iter.status = verify_ret;
			break;
		}

		/* Advance to next extent after successful verify */
		iter.status = iomap_iter_advance_full(&iter);

		cond_resched();
	}

out:
	return ret < 0 ? ret : iter.status;
}
EXPORT_SYMBOL_GPL(iomap_file_verify);
