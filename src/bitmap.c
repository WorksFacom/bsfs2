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
  struct buffer_head  *bh;
  int                  i;

  struct bsfs_super_info *sbi = bsfs_sb(sb);

  bitmap_map = kzalloc(sbi->super->block_bitmap_total, GFP_KERNEL);

  i = 0;

  __u32 current_block = sbi->super->block_bitmap_start;
  while (current_block != INVALID_BLOCK) {
    bitmap_block_t *block;

    // TODO: handle error
    bh    = sb_bread(sb, current_block);
    block = (bitmap_block_t *)bh->b_data;

    bitmap_map[i] = bh;

    current_block = block->next_block;
    i++;
  }

  sbi->bitmap_map = bitmap_map;
}

// int write_bitmap(struct super_block *sb, __u8 *bitmap, __u32 bitmap_start_block) {
//   uint32_t current_block = bitmap_start_block;
//   size_t   offset        = 0;
//
//   while (current_block != INVALID_BLOCK) {
//     bitmap_block_t block;
//     memset(&block, 0, sizeof(block));
//
//     // Preserve next_block pointer from on-disk structure
//     if (fseek(disk, current_block * BLOCK_SIZE + BITMAP_BLOCK_BYTES, SEEK_SET) != 0 ||
//         fread(&block.next_block, sizeof(uint32_t), 1, disk) != 1) {
//       fprintf(stderr, "Failed to read next block info.\n");
//       return -1;
//     }
//
//     // Copy a slice of the bitmap into this block's bits
//     size_t to_copy = BITMAP_BLOCK_BYTES;
//     memcpy(block.bits, bitmap + offset, to_copy);
//     offset += to_copy;
//
//     if (fseek(disk, current_block * BLOCK_SIZE, SEEK_SET) != 0 || fwrite(&block, sizeof(bitmap_block_t), 1, disk) != 1) {
//       fprintf(stderr, "Failed to write bitmap block.\n");
//       return -1;
//     }
//
//     current_block = block.next_block;
//   }
//
//   fflush(disk);
//   return 0;
// }

__u32 bsfs_is_bit_free(__u8 *bitmap, __u32 index) {
  uint32_t byte_index = index / 8;
  uint8_t  bit_index  = index % 8;

  return !(bitmap[byte_index] & (1 << bit_index));
}

void bsfs_set_bit(__u8 *bitmap, __u32 index) {
  uint32_t byte_index = index / 8;
  uint8_t  bit_index  = index % 8;

  bitmap[byte_index] |= (1 << bit_index);
}

void bsfs_clear_bit(__u8 *bitmap, __u32 index) {
  uint32_t byte_index = index / 8;
  uint8_t  bit_index  = index % 8;

  bitmap[byte_index] &= ~(1 << bit_index);
}
