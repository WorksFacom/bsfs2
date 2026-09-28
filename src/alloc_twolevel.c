/**
 * @file alloc_twolevel.c
 * @brief two-level index, a free-block count per fixed-size bucket.
 *
 * `make BSFS_ALLOC=twolevel`. port of bitmap_test_c/src/allocators/scatter/twolevel.c ("Two-Level Index",
 * fastest of the sixteen in the benchmarks).
 *
 * a summary array counts free blocks per BUCKET_SIZE bucket. alloc walks the summary (~96x shorter than
 * the bitmap) then searches one bucket a word at a time via bsfs_find_free_bit(). free is O(1).
 *
 * ## Versus the min-heap
 *
 * the heap indexes blocks, so memory grows with what was freed. this indexes regions, fixed at mount:
 * one byte per 96 blocks, ~667 KB for 250 GiB. the cost is ordering. you get the lowest free block in the
 * first bucket with room from the cursor, not the lowest overall. nothing depends on lowest-first.
 *
 * ## Differences from the userspace prototype
 *
 * the prototype starts empty and sets every bucket to BUCKET_SIZE. here the summary comes from one
 * bitmap pass at mount. the prototype also resets the cursor to 0 on every free, here it only moves
 * back to the freed bucket so known-full buckets aren't rescanned.
 *
 * ## Persistence
 *
 * none. _init() rebuilds the summary next mount, the bitmap is the only record.
 */
#include "../include/alloc_strategy.h"
#include "../include/bitmap.h"
#include <linux/build_bug.h>
#include <linux/mm.h>
#include <linux/slab.h>

/*
 * the prototype's 100 lands mid-byte and straddles bitmap blocks, so find_next_zero_bit_le() can't
 * search a bucket. powers of two don't work either, BITS_PER_BLOCK is 32736 = 2^5 x 3 x 11 x 31.
 * 96 is 12 bytes in one bh, 341 buckets per bitmap block.
 */
#define BUCKET_SIZE 96

typedef __u8 bucket_count_t;

static_assert(BUCKET_SIZE <= (bucket_count_t)~0u, "BUCKET_SIZE does not fit in bucket_count_t");

static_assert(BUCKET_SIZE % 8 == 0, "BUCKET_SIZE must be a whole number of bytes");

static_assert(BITS_PER_BLOCK % BUCKET_SIZE == 0, "BUCKET_SIZE must divide BITS_PER_BLOCK so buckets never straddle a bitmap block");

/**
 * @brief two-level strategy state, hung off sbi->alloc_private.
 *
 * @var free_in_bucket free count per bucket, kvmalloc'd at mount
 * @var num_buckets    covers block 0 onward so bucket = block / BUCKET_SIZE. buckets below data_block_start stay 0
 * @var cursor         bucket of the last alloc, probing starts here and wraps
 * @var probes         buckets examined, says whether the summary saves any work
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
 * clamped to data_block_start so we don't rely on mkfs having set the metadata bits.
 */
static inline void bucket_range(struct super_block *sb, __u32 bucket, __u32 *from, __u32 *to) {
  struct bsfs_super_info *sbi   = bsfs_sb(sb);
  __u32                   total = sbi->super->total_blocks;
  __u32                   start = bucket * BUCKET_SIZE;

  // start + BUCKET_SIZE can overflow near U32_MAX
  *to   = (total - start > BUCKET_SIZE) ? start + BUCKET_SIZE : total;
  *from = (start < sbi->super->data_block_start) ? sbi->super->data_block_start : start;
}

/*
 * same pass bsfs_init_block_counts() just made, kept separate since free_blocks belongs to the
 * allocator and every strategy needs it.
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
 * a positive count with no free bits means the summary drifted. the bitmap wins, fix the count and
 * keep probing.
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
 * pulling the cursor back keeps allocs low, otherwise space behind it waits for the next wrap.
 */
// caller holds sbi->bitmap_lock
void bsfs_alloc_strategy_freed(struct super_block *sb, __u32 block) {
  struct twolevel_alloc *t = bsfs_sb(sb)->alloc_private;
  __u32                  bucket = bucket_of(block);

  if (bucket >= t->num_buckets) {
    pr_warn_ratelimited("freed block %u is outside the %u buckets of this filesystem\n", block, t->num_buckets);
    return;
  }

  // callers only report real 1->0 transitions, the clamp is paranoia
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
