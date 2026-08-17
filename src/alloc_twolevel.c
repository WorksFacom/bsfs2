/**
 * @file alloc_twolevel.c
 * @brief Two-level index: a free-block count per fixed-size bucket.
 *
 * Selected by `make BSFS_ALLOC=twolevel`. Port of the `twolevel` strategy from
 * bitmap_test_c/src/allocators/scatter/twolevel.c ("Two-Level Index" in the
 * benchmarks, where it is the fastest of the sixteen).
 *
 * The bitmap is chopped into buckets of BUCKET_SIZE blocks and a summary array
 * holds how many blocks are free in each. Allocating walks the summary, which is
 * ~96x shorter than the bitmap, then searches bits only inside the one bucket it
 * settles on - and that search is itself a word at a time, see BUCKET_SIZE below
 * and bsfs_find_free_bit(). Freeing is O(1): bump the owning bucket's counter.
 *
 * ## How this differs from the min-heap
 *
 * The heap indexes *blocks* and so pays memory proportional to what has been
 * freed, which is unbounded in the worst case. This indexes *regions*, so its
 * memory is fixed at mount and proportional to the size of the filesystem:
 * one byte per 96 blocks, about 667 KB for a 250 GiB filesystem, no matter what
 * the workload does. That is the trade the two strategies exist to compare.
 *
 * What it gives up is the ordering guarantee. The heap always returns the lowest
 * free block, exactly like the linear scan; this returns the lowest free block
 * *in the first bucket it finds with room*, starting from a cursor that follows
 * the last allocation. Nothing in the filesystem depends on lowest-first - the
 * bitmap is the authority either way - but allocations do scatter differently.
 *
 * ## Where this differs from the userspace prototype
 *
 * The prototype starts from an empty bitmap and can therefore initialise every
 * bucket to BUCKET_SIZE. A mounted filesystem starts from whatever is on disk,
 * so the summary is built by one pass over the bitmap at mount - the same pass
 * bsfs_init_block_counts() already makes to count free blocks, and O(n) once
 * rather than per allocation.
 *
 * The prototype also resets its cursor to bucket 0 on every free, which makes
 * the next allocation rescan the summary from the start. Here the cursor is
 * pulled back only as far as the bucket that was freed, which reaches the same
 * blocks without rescanning what is already known to be full.
 *
 * ## What is not here
 *
 * Nothing persists. The summary is rebuilt by the scan in _init() on the next
 * mount, and the bitmap in the buffer heads stays the only record of what is
 * allocated.
 */
#include "../include/alloc_strategy.h"
#include "../include/bitmap.h"
#include <linux/build_bug.h>
#include <linux/mm.h>
#include <linux/slab.h>

/*
 * Blocks per bucket.
 *
 * The prototype uses 100, which is a fine number for a byte array but a poor one
 * for a packed bitmap: buckets land mid-byte and straddle bitmap blocks, so the
 * search inside a bucket cannot be handed to find_next_zero_bit_le().
 *
 * 96 is the value near it that satisfies both constraints below - the powers of
 * two do not, because BITS_PER_BLOCK is 32736 = 2^5 x 3 x 11 x 31 and so is
 * divisible by no power of two above 32. At 96 each bucket is exactly 12 bytes
 * inside a single buffer head, 341 buckets to a bitmap block, and one byte of
 * summary covers 96 blocks (~667 KB for a 250 GiB filesystem).
 */
#define BUCKET_SIZE 96

typedef __u8 bucket_count_t;

// one byte per bucket only works while a bucket cannot hold more blocks than
// that byte can count
static_assert(BUCKET_SIZE <= (bucket_count_t)~0u, "BUCKET_SIZE does not fit in bucket_count_t");

// a bucket that begins mid-byte cannot be searched as a byte range
static_assert(BUCKET_SIZE % 8 == 0, "BUCKET_SIZE must be a whole number of bytes");

// the bits of each bitmap block live in their own buffer head, so a bucket
// spanning two of them would take two searches instead of one
static_assert(BITS_PER_BLOCK % BUCKET_SIZE == 0, "BUCKET_SIZE must divide BITS_PER_BLOCK so buckets never straddle a bitmap block");

/**
 * @brief In-memory state of the two-level strategy, hung off sbi->alloc_private.
 *
 * @var free_in_bucket how many blocks are free in each bucket; kvmalloc'd,
 *                     never on disk, sized once at mount
 * @var num_buckets    covers block 0 to total_blocks, so a block's bucket is
 *                     just block / BUCKET_SIZE with no offset to apply. The
 *                     buckets below data_block_start simply stay at zero.
 * @var cursor         bucket the last allocation came from; probing starts here
 *                     and wraps
 * @var probes         buckets examined across all allocations, for the stats
 *                     line - the number that says whether the summary is
 *                     actually saving work
 */
struct twolevel_alloc {
  bucket_count_t *free_in_bucket;
  __u32           num_buckets;
  __u32           cursor;
  __u64           probes;
};

static inline __u32 bucket_of(__u32 block) {
  return block / BUCKET_SIZE;
}

/*
 * Blocks a bucket covers, clamped to the region the allocator may hand out.
 *
 * Metadata lives below data_block_start and its bits are set by mkfs, so a scan
 * would skip it anyway; clamping means the strategy does not have to depend on
 * that being true.
 */
static inline void bucket_range(struct super_block *sb, __u32 bucket, __u32 *from, __u32 *to) {
  struct bsfs_super_info *sbi   = bsfs_sb(sb);
  __u32                   total = sbi->super->total_blocks;
  __u32                   start = bucket * BUCKET_SIZE;

  // total - start rather than start + BUCKET_SIZE, which would overflow on a
  // filesystem whose last bucket ends within BUCKET_SIZE of U32_MAX
  *to   = (total - start > BUCKET_SIZE) ? start + BUCKET_SIZE : total;
  *from = (start < sbi->super->data_block_start) ? sbi->super->data_block_start : start;
}

/*
 * Builds the summary from the bitmap.
 *
 * One pass over every block, at mount only. bsfs_init_block_counts() has just
 * made the same pass to compute sbi->free_blocks; they are kept separate because
 * that count is the allocator's, not the strategy's, and every strategy needs it
 * whether or not it needs a summary.
 */
int bsfs_alloc_strategy_init(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  struct twolevel_alloc  *t;
  __u32                   i, total = sbi->super->total_blocks;

  t = kzalloc(sizeof(*t), GFP_KERNEL);

  if (!t)
    return -ENOMEM;

  t->num_buckets = DIV_ROUND_UP(total, BUCKET_SIZE);

  if (t->num_buckets == 0) {
    kfree(t);
    pr_err("filesystem declares %u blocks, nothing to allocate from\n", total);
    return -EUCLEAN;
  }

  t->free_in_bucket = kvcalloc(t->num_buckets, sizeof(*t->free_in_bucket), GFP_KERNEL);

  if (!t->free_in_bucket) {
    kfree(t);
    return -ENOMEM;
  }

  for (i = sbi->super->data_block_start; i < total; i++) {
    if (bsfs_is_bit_free(sb, i))
      t->free_in_bucket[bucket_of(i)]++;
  }

  t->cursor          = bucket_of(sbi->super->data_block_start);
  sbi->alloc_private = t;

  BSFS_STAT_MAX(sb, index_peak, (__u64)t->num_buckets);

  return 0;
}

void bsfs_alloc_strategy_destroy(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  struct twolevel_alloc  *t   = sbi->alloc_private;

  if (!t)
    return;

  kvfree(t->free_in_bucket);
  kfree(t);
  sbi->alloc_private = NULL;
}

/*
 * Walks the summary from the cursor, wrapping once, and takes the first free
 * block in the first bucket that claims to have one.
 *
 * A bucket whose counter is positive but whose blocks are all taken means the
 * summary has drifted from the bitmap. The bitmap wins - it is the authority -
 * so the counter is corrected and the probe continues. Handing out a block the
 * bitmap says is allocated is what gives two owners the same block.
 */
// caller holds sbi->bitmap_lock
__u32 bsfs_alloc_strategy_alloc(struct super_block *sb) {
  struct twolevel_alloc *t = bsfs_sb(sb)->alloc_private;
  __u32                  probe;

  for (probe = 0; probe < t->num_buckets; probe++) {
    __u32 bucket = t->cursor + probe;
    __u32 from, to, hit;

    // cursor and probe are both < num_buckets, so one subtraction wraps
    if (bucket >= t->num_buckets)
      bucket -= t->num_buckets;

    t->probes++;

    if (t->free_in_bucket[bucket] == 0)
      continue;

    bucket_range(sb, bucket, &from, &to);
    hit = bsfs_find_free_bit(sb, from, to);

    if (hit < to) {
      bsfs_set_bit(sb, hit);
      t->free_in_bucket[bucket]--;
      t->cursor = bucket;

      BSFS_STAT_INC(sb, from_index);
      return hit;
    }

    pr_warn_ratelimited("bucket %u claimed %u free blocks but has none, correcting\n", bucket, t->free_in_bucket[bucket]);
    t->free_in_bucket[bucket] = 0;
  }

  return INVALID_BLOCK;
}

/*
 * O(1), and the reason the summary is worth keeping.
 *
 * Pulling the cursor back to the freed bucket is what keeps allocation near the
 * low end of the device: without it the cursor only ever moves forward and space
 * released behind it would not be reused until the next wrap.
 */
// caller holds sbi->bitmap_lock
void bsfs_alloc_strategy_freed(struct super_block *sb, __u32 block) {
  struct twolevel_alloc *t = bsfs_sb(sb)->alloc_private;
  __u32                  bucket = bucket_of(block);

  if (bucket >= t->num_buckets) {
    pr_warn_ratelimited("freed block %u is outside the %u buckets of this filesystem\n", block, t->num_buckets);
    return;
  }

  // the caller only reports real 1->0 transitions, so this cannot overcount past
  // the number of blocks the bucket holds
  if (t->free_in_bucket[bucket] < BUCKET_SIZE)
    t->free_in_bucket[bucket]++;

  if (bucket < t->cursor)
    t->cursor = bucket;
}

void bsfs_alloc_strategy_stats(struct super_block *sb, char *buf, size_t len) {
  struct twolevel_alloc *t = bsfs_sb(sb)->alloc_private;

  if (!t) {
    if (len)
      buf[0] = '\0';
    return;
  }

  snprintf(buf, len, "buckets=%u bucket_size=%u aux_bytes=%zu probes=%llu cursor=%u", t->num_buckets, BUCKET_SIZE,
           (size_t)t->num_buckets * sizeof(*t->free_in_bucket), t->probes, t->cursor);
}
