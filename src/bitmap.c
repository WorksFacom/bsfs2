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

// int init_bitmap_list(struct super_block *sb, __u32 start_block, __u32 num_blocks) {
//   for (uint32_t i = 0; i < num_blocks; i++) {
//     bitmap_block_t block;
//     memset(block.bits, 0, BITMAP_BLOCK_BYTES);
//     if (i < num_blocks - 1) {
//       block.next_block = start_block + i + 1;
//     } else {
//       block.next_block = INVALID_BLOCK;
//     }
//
//     if (fseek(disk, (start_block + i) * BLOCK_SIZE, SEEK_SET) != 0) {
//       fprintf(stderr, "Failed to seek while initializing bitmap list.\n");
//       return -1;
//     }
//     if (fwrite(&block, sizeof(bitmap_block_t), 1, disk) != 1) {
//       fprintf(stderr, "Failed to write bitmap block.\n");
//       return -1;
//     }
//   }
//   fflush(disk);
//   return 0;
// }

void load_bitmap(struct super_block *sb) {
  struct buffer_head **bitmap_map;
  __u8               **bitmap;
  struct buffer_head  *bh;
  int                  i;

  struct bsfs_super_info *sbi = bsfs_sb(sb);

  bitmap_map = kcalloc(sbi->super->block_bitmap_total, sizeof(*bitmap_map), GFP_KERNEL);
  bitmap     = kcalloc(sbi->super->block_bitmap_total, sizeof(*bitmap), GFP_KERNEL);

  i = 0;

  __u32 current_block = sbi->super->block_bitmap_start;
  while (current_block != INVALID_BLOCK) {
    bitmap_block_t *block;

    // TODO: handle error
    bh    = sb_bread(sb, current_block);
    block = (bitmap_block_t *)bh->b_data;

    bitmap_map[i] = bh;
    bitmap[i]     = block->bits;

    current_block = block->next_block;
    i++;
  }

  sbi->bitmap     = bitmap;
  sbi->bitmap_map = bitmap_map;
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
