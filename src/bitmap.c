/*
 * @file bitmap_operations_bsfs.c
 * @brief C file containing functions for bitmap manipulation in the BSFS
 * file system.
 *
 * This file provides functions for loading, saving, and manipulating the bitmap
 * which is used for managing free and allocated blocks in the filesystem.
 */
#include "../include/bitmap.h"
#include "../bsfs.h"

int load_bitmap(struct super_block *sb) {
  struct buffer_head **bitmap_map;
  __u8               **bitmap;
  __u32                i, current_block;

  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 total = sbi->super->block_bitmap_total;

  // bsfs_is_bit_free() and friends index sbi->bitmap[] straight from a block
  // number, so a superblock claiming fewer bitmap blocks than it has blocks to
  // describe turns every allocation into an out-of-bounds access.
  if (total == 0 || total < DIV_ROUND_UP(sbi->super->total_blocks, BITS_PER_BLOCK)) {
    pr_err("superblock declares %u bitmap blocks, too few for %u blocks\n", total, sbi->super->total_blocks);
    return -EUCLEAN;
  }

  bitmap_map = kcalloc(total, sizeof(*bitmap_map), GFP_KERNEL);
  bitmap     = kcalloc(total, sizeof(*bitmap), GFP_KERNEL);

  if (!bitmap_map || !bitmap) {
    kfree(bitmap_map);
    kfree(bitmap);
    return -ENOMEM;
  }

  // published before the loop so release_bitmap() can clean up a partial load
  sbi->bitmap     = bitmap;
  sbi->bitmap_map = bitmap_map;

  current_block = sbi->super->block_bitmap_start;

  // next_block comes off the disk, so the walk is bounded by the count the
  // superblock declares instead of trusting the chain to terminate
  for (i = 0; i < total; i++) {
    bitmap_block_t     *block;
    struct buffer_head *bh;

    // has to stay inside the region the superblock set aside for the bitmap;
    // "anywhere on the device" would happily read block 0 or a data block and
    // then hand out allocations from whatever was in it
    if (current_block < sbi->super->block_bitmap_start || current_block - sbi->super->block_bitmap_start >= total) {
      pr_err("bitmap chain leaves the bitmap region at block %u (%u of %u loaded)\n", current_block, i, total);
      return -EUCLEAN;
    }

    bh = sb_bread(sb, current_block);

    if (!bh) {
      pr_err("unable to read bitmap block %u\n", current_block);
      return -EIO;
    }

    block = (bitmap_block_t *)bh->b_data;

    bitmap_map[i] = bh;
    bitmap[i]     = block->bits;

    current_block = block->next_block;
  }

  return 0;
}

/*
 * Counterpart of load_bitmap(): the buffer heads it pinned (one per bitmap
 * block) and the two arrays were never released, so every mount leaked them.
 */
void release_bitmap(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   i;

  if (!sbi->bitmap_map)
    goto out;

  for (i = 0; i < sbi->super->block_bitmap_total; i++) {
    if (sbi->bitmap_map[i]) {
      // The bits live inside these buffers, so flush what is still dirty before
      // letting go of them.
      if (buffer_dirty(sbi->bitmap_map[i]))
        sync_dirty_buffer(sbi->bitmap_map[i]);

      brelse(sbi->bitmap_map[i]);
    }
  }

out:
  kfree(sbi->bitmap_map);
  kfree(sbi->bitmap);
  sbi->bitmap_map = NULL;
  sbi->bitmap     = NULL;
}
