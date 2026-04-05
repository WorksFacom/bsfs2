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
  struct __u8        **bitmap;
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

// TODO: are these 3 functions below working??
static inline __u32 bsfs_is_bit_free(struct super_block *sb, __u32 index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 block  = index / BITS_PER_BLOCK;
  __u32 offset = index % BITS_PER_BLOCK;

  __u8 *bits = sbi->bitmap[block];

  return !(bits[offset / 8] & (1U << (offset % 8)));
}

static inline void bsfs_set_bit(struct super_block *sb, __u32 index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 block  = index / BITS_PER_BLOCK;
  __u32 offset = index % BITS_PER_BLOCK;

  __u8 *bits = sbi->bitmap[block];

  bits[offset / 8] |= (1U << (offset % 8));

  mark_buffer_dirty(sbi->bitmap_map[block]);
}

static inline void bsfs_clear_bit(struct super_block *sb, __u32 index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 block  = index / BITS_PER_BLOCK;
  __u32 offset = index % BITS_PER_BLOCK;

  __u8 *bits = sbi->bitmap[block];

  bits[offset / 8] &= ~(1U << (offset % 8));

  mark_buffer_dirty(sbi->bitmap_map[block]);
}
