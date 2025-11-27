// SPDX-License-Identifier: GPL-2.0
/*
 * Functions related to generic helpers functions
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/scatterlist.h>

#include "blk.h"

static sector_t bio_discard_limit(struct block_device *bdev, sector_t sector)
{
	unsigned int discard_granularity = bdev_discard_granularity(bdev);
	sector_t granularity_aligned_sector;

	if (bdev_is_partition(bdev))
		sector += bdev->bd_start_sect;

	granularity_aligned_sector =
		round_up(sector, discard_granularity >> SECTOR_SHIFT);

	/*
	 * Make sure subsequent bios start aligned to the discard granularity if
	 * it needs to be split.
	 */
	if (granularity_aligned_sector != sector)
		return granularity_aligned_sector - sector;

	/*
	 * Align the bio size to the discard granularity to make splitting the bio
	 * at discard granularity boundaries easier in the driver if needed.
	 */
	return round_down(UINT_MAX, discard_granularity) >> SECTOR_SHIFT;
}

struct bio *blk_alloc_discard_bio(struct block_device *bdev,
		sector_t *sector, sector_t *nr_sects, gfp_t gfp_mask)
{
	sector_t bio_sects = min(*nr_sects, bio_discard_limit(bdev, *sector));
	struct bio *bio;

	if (!bio_sects)
		return NULL;

	bio = bio_alloc(bdev, 0, REQ_OP_DISCARD, gfp_mask);
	if (!bio)
		return NULL;
	bio->bi_iter.bi_sector = *sector;
	bio->bi_iter.bi_size = bio_sects << SECTOR_SHIFT;
	*sector += bio_sects;
	*nr_sects -= bio_sects;
	/*
	 * We can loop for a long time in here if someone does full device
	 * discards (like mkfs).  Be nice and allow us to schedule out to avoid
	 * softlocking if preempt is disabled.
	 */
	cond_resched();
	return bio;
}

int __blkdev_issue_discard(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, struct bio **biop)
{
	struct bio *bio;

	while ((bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects,
			gfp_mask)))
		*biop = bio_chain_and_submit(*biop, bio);
	return 0;
}
EXPORT_SYMBOL(__blkdev_issue_discard);

/**
 * blkdev_issue_discard - queue a discard
 * @bdev:	blockdev to issue discard for
 * @sector:	start sector
 * @nr_sects:	number of sectors to discard
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Issue a discard request for the sectors in question.
 */
int blkdev_issue_discard(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask)
{
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	blk_start_plug(&plug);
	__blkdev_issue_discard(bdev, sector, nr_sects, gfp_mask, &bio);
	if (bio) {
		ret = submit_bio_wait(bio);
		if (ret == -EOPNOTSUPP)
			ret = 0;
		bio_put(bio);
	}
	blk_finish_plug(&plug);

	return ret;
}
EXPORT_SYMBOL(blkdev_issue_discard);

static sector_t bio_write_zeroes_limit(struct block_device *bdev)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1;

	return min(bdev_write_zeroes_sectors(bdev),
		(UINT_MAX >> SECTOR_SHIFT) & ~bs_mask);
}

/*
 * Allocate a bio for payloadless operations that don't transfer data, such as
 * REQ_OP_WRITE_ZEROES or REQ_OP_VERIFY. The bio carries only sector range
 * information and no data pages.
 */
static struct bio *blk_alloc_payloadless_bio(struct block_device *bdev,
		sector_t *sector, sector_t *nr_sects, sector_t max_sectors,
		enum req_op op, gfp_t gfp_mask)
{
	sector_t len;
	struct bio *bio;

	if (!*nr_sects)
		return NULL;

	len = min(*nr_sects, max_sectors);

	bio = bio_alloc(bdev, 0, op, gfp_mask);
	if (!bio)
		return NULL;

	bio->bi_iter.bi_sector = *sector;
	bio->bi_iter.bi_size = len << SECTOR_SHIFT;

	*sector += len;
	*nr_sects -= len;

	cond_resched();
	return bio;
}

/*
 * There is no reliable way for the SCSI subsystem to determine whether a
 * device supports a WRITE SAME operation without actually performing a write
 * to media. As a result, write_zeroes is enabled by default and will be
 * disabled if a zeroing operation subsequently fails. This means that this
 * queue limit is likely to change at runtime.
 */
static void __blkdev_issue_write_zeroes(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned flags, sector_t limit)
{
	struct bio *bio;

	while (nr_sects) {
		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current))
			break;

		bio = blk_alloc_payloadless_bio(bdev, &sector, &nr_sects, limit,
				REQ_OP_WRITE_ZEROES, gfp_mask);
		if (!bio)
			break;
		if (flags & BLKDEV_ZERO_NOUNMAP)
			bio->bi_opf |= REQ_NOUNMAP;

		*biop = bio_chain_and_submit(*biop, bio);
	}
}

static int blkdev_issue_write_zeroes(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp, unsigned flags)
{
	sector_t limit = bio_write_zeroes_limit(bdev);
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	blk_start_plug(&plug);
	__blkdev_issue_write_zeroes(bdev, sector, nr_sects, gfp, &bio,
			flags, limit);
	if (bio) {
		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current)) {
			bio_await_chain(bio);
			blk_finish_plug(&plug);
			return -EINTR;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
	}
	blk_finish_plug(&plug);

	/*
	 * For some devices there is no non-destructive way to verify whether
	 * WRITE ZEROES is actually supported.  These will clear the capability
	 * on an I/O error, in which case we'll turn any error into
	 * "not supported" here.
	 */
	if (ret && !bdev_write_zeroes_sectors(bdev))
		return -EOPNOTSUPP;
	return ret;
}

/*
 * Convert a number of 512B sectors to a number of pages.
 * The result is limited to a number of pages that can fit into a BIO.
 * Also make sure that the result is always at least 1 (page) for the cases
 * where nr_sects is lower than the number of sectors in a page.
 */
static unsigned int __blkdev_sectors_to_bio_pages(sector_t nr_sects)
{
	sector_t pages = DIV_ROUND_UP_SECTOR_T(nr_sects, PAGE_SIZE / 512);

	return min(pages, (sector_t)BIO_MAX_VECS);
}

/*
 * Issue read or write bios for a block range using a provided folio.
 * The same folio is reused across all bios - suitable for writing zero pages
 * or read-and-discard verify operations.
 */
static void __blkdev_issue_rw(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, struct folio *folio, enum req_op op)
{
	while (nr_sects) {
		unsigned int nr_vecs = __blkdev_sectors_to_bio_pages(nr_sects);
		struct bio *bio;

		bio = bio_alloc(bdev, nr_vecs, op, gfp_mask);
		bio->bi_iter.bi_sector = sector;

		do {
			unsigned int len;

			len = min_t(sector_t, folio_size(folio),
				    nr_sects << SECTOR_SHIFT);
			if (!bio_add_folio(bio, folio, len, 0))
				break;
			nr_sects -= len >> SECTOR_SHIFT;
			sector += len >> SECTOR_SHIFT;
		} while (nr_sects);

		*biop = bio_chain_and_submit(*biop, bio);
		cond_resched();
	}
}

static void __blkdev_issue_zero_pages(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned int flags)
{
	struct folio *zero_folio = largest_zero_folio();

	while (nr_sects) {
		unsigned int nr_vecs = __blkdev_sectors_to_bio_pages(nr_sects);
		struct bio *bio;

		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current))
			break;

		bio = bio_alloc(bdev, nr_vecs, REQ_OP_WRITE, gfp_mask);
		bio->bi_iter.bi_sector = sector;

		do {
			unsigned int len;

			len = min_t(sector_t, folio_size(zero_folio),
				    nr_sects << SECTOR_SHIFT);
			if (!bio_add_folio(bio, zero_folio, len, 0))
				break;
			nr_sects -= len >> SECTOR_SHIFT;
			sector += len >> SECTOR_SHIFT;
		} while (nr_sects);

		*biop = bio_chain_and_submit(*biop, bio);
		cond_resched();
	}
}

static int blkdev_issue_zero_pages(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp, unsigned flags)
{
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	if (flags & BLKDEV_ZERO_NOFALLBACK)
		return -EOPNOTSUPP;

	blk_start_plug(&plug);
	__blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp, &bio, flags);
	if (bio) {
		if ((flags & BLKDEV_ZERO_KILLABLE) &&
		    fatal_signal_pending(current)) {
			bio_await_chain(bio);
			blk_finish_plug(&plug);
			return -EINTR;
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
	}
	blk_finish_plug(&plug);

	return ret;
}

/**
 * __blkdev_issue_zeroout - generate number of zero filed write bios
 * @bdev:	blockdev to issue
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @biop:	pointer to anchor bio
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.
 *
 *  If a device is using logical block provisioning, the underlying space will
 *  not be released if %flags contains BLKDEV_ZERO_NOUNMAP.
 *
 *  If %flags contains BLKDEV_ZERO_NOFALLBACK, the function will return
 *  -EOPNOTSUPP if no explicit hardware offload for zeroing is provided.
 */
int __blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, struct bio **biop,
		unsigned flags)
{
	sector_t limit = bio_write_zeroes_limit(bdev);

	if (bdev_read_only(bdev))
		return -EPERM;

	if (limit) {
		__blkdev_issue_write_zeroes(bdev, sector, nr_sects,
				gfp_mask, biop, flags, limit);
	} else {
		if (flags & BLKDEV_ZERO_NOFALLBACK)
			return -EOPNOTSUPP;
		__blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp_mask,
				biop, flags);
	}
	return 0;
}
EXPORT_SYMBOL(__blkdev_issue_zeroout);

/**
 * blkdev_issue_zeroout - zero-fill a block range
 * @bdev:	blockdev to write
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.  See __blkdev_issue_zeroout() for the
 *  valid values for %flags.
 */
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, unsigned flags)
{
	int ret;

	if ((sector | nr_sects) & ((bdev_logical_block_size(bdev) >> 9) - 1))
		return -EINVAL;
	if (bdev_read_only(bdev))
		return -EPERM;

	if (bdev_write_zeroes_sectors(bdev)) {
		ret = blkdev_issue_write_zeroes(bdev, sector, nr_sects,
				gfp_mask, flags);
		if (ret != -EOPNOTSUPP)
			return ret;
	}

	return blkdev_issue_zero_pages(bdev, sector, nr_sects, gfp_mask, flags);
}
EXPORT_SYMBOL(blkdev_issue_zeroout);

int blkdev_issue_secure_erase(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1;
	unsigned int max_sectors = bdev_max_secure_erase_sectors(bdev);
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;

	/* make sure that "len << SECTOR_SHIFT" doesn't overflow */
	if (max_sectors > UINT_MAX >> SECTOR_SHIFT)
		max_sectors = UINT_MAX >> SECTOR_SHIFT;
	max_sectors &= ~bs_mask;

	if (max_sectors == 0)
		return -EOPNOTSUPP;
	if ((sector | nr_sects) & bs_mask)
		return -EINVAL;
	if (bdev_read_only(bdev))
		return -EPERM;

	blk_start_plug(&plug);
	while (nr_sects) {
		unsigned int len = min_t(sector_t, nr_sects, max_sectors);

		bio = blk_next_bio(bio, bdev, 0, REQ_OP_SECURE_ERASE, gfp);
		bio->bi_iter.bi_sector = sector;
		bio->bi_iter.bi_size = len << SECTOR_SHIFT;

		sector += len;
		nr_sects -= len;
		cond_resched();
	}
	if (bio) {
		ret = submit_bio_wait(bio);
		bio_put(bio);
	}
	blk_finish_plug(&plug);

	return ret;
}
EXPORT_SYMBOL(blkdev_issue_secure_erase);

/*
 * Issue read bios to verify data can be read from the device.
 * Data is read into a temporary folio and discarded - this is used
 * as a fallback when hardware verify (REQ_OP_VERIFY) is not supported.
 */
static int blkdev_issue_read_pages(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp)
{
	sector_t chunk_size = bdev->bd_queue->limits.verify_chunk_sectors;
	struct folio *folio;
	struct bio *bio = NULL;
	struct blk_plug plug;
	int ret = 0;
	sector_t chunk;

	pr_debug("VER_DBG: %-30s [read-based fallback]: dev=%pg sector=%llu "
		 "nr_sects=%llu\n", __func__, bdev, (unsigned long long)sector,
		 (unsigned long long)nr_sects);

	/*
	 * Allocate a large folio (2MB) to reduce memory allocation overhead.
	 * The folio serves as a dummy data buffer that will be reused across
	 * all bios. Individual bios are still limited by the device's
	 * max_sectors (typically 512KB), so this doesn't directly affect
	 * bio size. The main benefit is avoiding repeated small allocations.
	 */
	folio = folio_alloc(gfp, 9); /* order 9 = 2MB */
	if (!folio)
		folio = folio_alloc(gfp, 4); /* fallback: 64KB */
	if (!folio)
		folio = folio_alloc(gfp, 0); /* fallback: 4KB */
	if (!folio)
		return -ENOMEM;

	pr_debug("VER_DBG: %-30s folio=%luKB chunk=%lluKB (%llu sectors)\n",
		 __func__, folio_size(folio) >> 10,
		 (unsigned long long)(chunk_size >> 1),
		 (unsigned long long)chunk_size);

	/*
	 * Process in chunks to avoid overwhelming the device queue.
	 * Submit chunk_size worth of I/Os, wait for completion, then
	 * submit the next chunk. This prevents queue flooding on devices
	 * with limited queue depth.
	 */
	while (nr_sects) {
		chunk = min(nr_sects, chunk_size);
		bio = NULL;

		blk_start_plug(&plug);
		__blkdev_issue_rw(bdev, sector, chunk, gfp, &bio, folio,
				  REQ_OP_READ);
		if (bio) {
			ret = submit_bio_wait(bio);
			bio_put(bio);
		}
		blk_finish_plug(&plug);

		if (ret)
			break;

		sector += chunk;
		nr_sects -= chunk;
	}

	folio_put(folio);
	return ret;
}

static sector_t bio_verify_limit(struct block_device *bdev)
{
	sector_t bs_mask = (bdev_logical_block_size(bdev) >> 9) - 1;

	return min(bdev_verify_sectors(bdev),
		(UINT_MAX >> SECTOR_SHIFT) & ~bs_mask);
}

static void __blkdev_issue_verify(struct block_device *bdev,
		sector_t sector, sector_t nr_sects, gfp_t gfp_mask,
		struct bio **biop, unsigned int flags, sector_t limit)
{
	struct bio *bio;

	while ((bio = blk_alloc_payloadless_bio(bdev, &sector, &nr_sects,
					limit, REQ_OP_VERIFY, gfp_mask))) {
		*biop = bio_chain_and_submit(*biop, bio);
	}
}

/*
 * blkdev_issue_verify_hw - Issue hardware verify with queue throttling
 *
 * Uses REQ_OP_VERIFY to verify data via hardware offload. Implements
 * batching via verify_chunk_sectors to prevent queue flooding.
 *
 * Verify chunk handling:
 *   1. Divide total operation into chunks of verify_chunk_sectors
 *   2. For each chunk: submit all I/Os via __blkdev_issue_verify()
 *   3. Wait for chunk completion via submit_bio_wait()
 *   4. Proceed to next chunk
 *
 * This prevents submitting thousands of I/Os at once, which can overwhelm
 * devices with limited queue depth. Within each chunk, I/Os are still
 * submitted in parallel for performance.
 *
 * Example: 1GB verify with 512KB I/Os and 32MB chunks:
 *   - Total I/Os: 2048 (1GB ÷ 512KB)
 *   - I/Os per chunk: 64 (32MB ÷ 512KB)
 *   - Total chunks: 32 (1GB ÷ 32MB)
 *   - Pattern: Submit 64 I/Os -> wait -> submit 64 I/Os -> wait (repeat 32x)
 */
static int blkdev_issue_verify_hw(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, unsigned int flags,
		sector_t limit)
{
	struct bio *bio = NULL;
	struct blk_plug plug;
	sector_t chunk_size = bdev->bd_queue->limits.verify_chunk_sectors;
	sector_t chunk;
	int ret = 0;

	pr_debug("VER_DBG: %-30s [hardware verify path]: dev=%pg sector=%llu "
		  "nr_sects=%llu limit=%llu chunk_size=%llu\n",
		  __func__, bdev, (unsigned long long)sector,
		  (unsigned long long)nr_sects, (unsigned long long)limit,
		  (unsigned long long)chunk_size);

	/*
	 * Process in chunks to avoid overwhelming the queue.
	 * Each iteration submits chunk_size worth of I/Os and waits for
	 * completion before proceeding to the next chunk.
	 */
	while (nr_sects) {
		chunk = min(nr_sects, chunk_size);
		bio = NULL;

		blk_start_plug(&plug);
		__blkdev_issue_verify(bdev, sector, chunk, gfp_mask, &bio,
				flags, limit);
		if (bio) {
			ret = submit_bio_wait(bio);
			bio_put(bio);
		}
		blk_finish_plug(&plug);

		if (ret)
			break;

		sector += chunk;
		nr_sects -= chunk;
	}

	/*
	 * For some devices there is no non-destructive way to verify whether
	 * REQ_OP_VERIFY is actually supported.  These will clear the capability
	 * on an I/O error, in which case we'll turn any error into
	 * "not supported" here.
	 */
	if (ret && !bdev_verify_sectors(bdev))
		return -EOPNOTSUPP;
	return ret;
}

/**
 * blkdev_issue_verify - verify a block range
 * @bdev:	blockdev to verify
 * @sector:	start sector
 * @nr_sects:	number of sectors to verify
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @flags:	BLKDEV_VERIFY_* flags to control behaviour
 *
 * Description:
 *    Issue a verify request for the sectors in question, either using
 *    hardware offload or by falling back to reading the data.
 *
 *    If %flags contains BLKDEV_VERIFY_NOFALLBACK, the function will return
 *    -EOPNOTSUPP if no explicit hardware offload for verify is provided.
 */
int blkdev_issue_verify(struct block_device *bdev, sector_t sector,
		sector_t nr_sects, gfp_t gfp_mask, unsigned int flags)
{
	sector_t limit = bio_verify_limit(bdev);
	int ret;

	if ((sector | nr_sects) & ((bdev_logical_block_size(bdev) >> 9) - 1))
		return -EINVAL;

	if (limit) {
		ret = blkdev_issue_verify_hw(bdev, sector, nr_sects, gfp_mask,
					     flags, limit);
		if (ret != -EOPNOTSUPP)
			return ret;
	}

	if (flags & BLKDEV_VERIFY_NOFALLBACK)
		return -EOPNOTSUPP;

	return blkdev_issue_read_pages(bdev, sector, nr_sects, gfp_mask);
}
EXPORT_SYMBOL(blkdev_issue_verify);
