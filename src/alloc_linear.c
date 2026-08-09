/**
 * @file alloc_linear.c
 * @brief First-fit linear scan - the historical BSFS allocator.
 *
 * Selected by `make` with no arguments, or `make BSFS_ALLOC=linear`.
 *
 * Every allocation walks the bitmap from data_block_start and takes the first
 * free bit: O(n) in the size of the filesystem, per block, with no cursor and no
 * memory of where the last one came from. That is the whole strategy, and it is
 * kept here unchanged because it is the reference the others are compared
 * against - both for behaviour (the QEMU suite must produce identical results)
 * and for speed (it is `baseline` in bitmap_test_c/).
 *
 * It needs no index at all, so init/destroy are no-ops and there is no
 * sbi->alloc_private.
 */
#include "../include/alloc_strategy.h"
#include "../include/bitmap.h"

int bsfs_alloc_strategy_init(struct super_block *sb) {
  (void)sb;
  return 0;
}

void bsfs_alloc_strategy_destroy(struct super_block *sb) {
  (void)sb;
}

// caller holds sbi->bitmap_lock
__u32 bsfs_alloc_strategy_alloc(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 i;
  __u32 start        = sbi->super->data_block_start;
  __u32 total_blocks = sbi->super->total_blocks;

  for (i = start; i < total_blocks; i++) {
    if (bsfs_is_bit_free(sb, i)) {
      bsfs_set_bit(sb, i);
      BSFS_STAT_INC(sb, from_cursor);
      return i;
    }
  }

  return INVALID_BLOCK;
}

/*
 * Nothing to record: the scan finds the block again on its own.
 */
// caller holds sbi->bitmap_lock
void bsfs_alloc_strategy_freed(struct super_block *sb, __u32 block) {
  (void)sb;
  (void)block;
}

void bsfs_alloc_strategy_stats(struct super_block *sb, char *buf, size_t len) {
  (void)sb;

  if (len)
    buf[0] = '\0';
}
