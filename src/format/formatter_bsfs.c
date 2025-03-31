// Formatter of the BSFS
#include "../include/filesystem_bsfs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Write the superblock
void write_superblock(FILE *file, superblock_t *sb) {
  fseek(file, 0, SEEK_SET);
  fwrite(sb, sizeof(superblock_t), 1, file);
}

// Bit manipulation
void bitmap_bit_toggle(FILE *file, uint32_t bitmap_start_block,
                       uint32_t bitmap_end_block, uint32_t bit_index) {
  uint32_t byte_index = bit_index / 8;     // Byte containing the bit to flip
  uint32_t byte_bit_index = bit_index % 8; // Index of the bit in the byte
  uint32_t block_offset = byte_index / BLOCK_SIZE; // Bitmap block number
  uint32_t block_index = bitmap_start_block + block_offset;
  uint32_t byte_offset = byte_index % BLOCK_SIZE;

  if (block_index > bitmap_end_block) {
    fprintf(stderr, "Error: bit number out of range\n");
    return;
  }

  fseek(file, (block_index * BLOCK_SIZE) + byte_offset, SEEK_SET);
  uint8_t byte;
  fread(&byte, 1, 1, file);
  byte ^= (1 << byte_bit_index);
  fseek(file, -1, SEEK_CUR);
  fwrite(&byte, 1, 1, file);
  fflush(file);
}

// Create and initializes the root directory
void create_root_directory(FILE *file, superblock_t *sb) {
  inode_t root_inode = {.file_size = sizeof(directory_entry_t),
                        .file_type = 2,
                        .block_count = 1,
                        .uid = 0,
                        .gid = 0,
                        .permissions = 0755,
                        .links_count = 1,
                        .created_at = (uint32_t)time(NULL),
                        .modified_at = (uint32_t)time(NULL),
                        .accessed_at = (uint32_t)time(NULL),
                        .direct_blocks[0] = sb->data_block_start};
  fseek(file, sb->inode_blocks_start * BLOCK_SIZE, SEEK_SET);
  fwrite(&root_inode, sizeof(inode_t), 1, file);

  // Write dot entry
  directory_entry_t dot_entry = {
      .inode_number = 1,
      .entry_size = sizeof(directory_entry_t),
      .name_size = 1,
      .file_type = 2,
  };
  strcpy(dot_entry.filename, ".");

  fseek(file, sb->data_block_start * BLOCK_SIZE, SEEK_SET);
  fwrite(&dot_entry, sizeof(directory_entry_t), 1, file);

  // Write dot dot entry
  directory_entry_t dot_dot_entry = {
      .inode_number = 1,
      .entry_size = sizeof(directory_entry_t),
      .name_size = 2,
      .file_type = 2,
  };
  strcpy(dot_dot_entry.filename, "..");

  fseek(file, sb->data_block_start * BLOCK_SIZE + sizeof(directory_entry_t),
        SEEK_SET);
  fwrite(&dot_dot_entry, sizeof(directory_entry_t), 1, file);
}

// Main
int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <output file>\n", argv[0]);
    return EXIT_FAILURE;
  }

  FILE *file = fopen(argv[1], "rb+");
  if (!file) {
    file = fopen(argv[1], "wb+");
    if (!file) {
      perror("Error opening file");
      return EXIT_FAILURE;
    }
  }

  fseek(file, 0, SEEK_END);
  uint32_t fs_size = ftell(file);
  if (fs_size == 0) {
    fprintf(stderr,
            "Error: Filesystem file is empty or not properly created.\n");
    fclose(file);
    return EXIT_FAILURE;
  }

  uint32_t total_blocks = fs_size / BLOCK_SIZE;
  uint32_t total_inodes = total_blocks;
  uint32_t inode_bitmap_blocks =
      (total_inodes + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
  uint32_t block_bitmap_blocks =
      (total_inodes + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
  uint32_t rec_bitmap_blocks =
      (total_inodes + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
  uint32_t inode_blocks = (total_inodes * INODE_SIZE) / BLOCK_SIZE;
  uint32_t data_blocks =
      total_blocks - (1 + inode_bitmap_blocks + block_bitmap_blocks +
                      rec_bitmap_blocks + inode_blocks);

  superblock_t sb = {
      .magic_number = MAGIC_NUMBER,
      .block_size = BLOCK_SIZE,
      .fs_size = fs_size,
      .total_inodes = total_inodes,
      .total_blocks = total_blocks,
      .free_inodes = total_inodes,
      .free_blocks = data_blocks,
      .inode_bitmap_start = 2,
      .inode_bitmap_total = inode_bitmap_blocks,
      .block_bitmap_start = 1 + inode_bitmap_blocks,
      .block_bitmap_total = block_bitmap_blocks,
      .rec_bitmap_start = 1 + inode_bitmap_blocks + block_bitmap_blocks,
      .rec_bitmap_total = rec_bitmap_blocks,
      .inode_blocks_start =
          1 + inode_bitmap_blocks + block_bitmap_blocks + rec_bitmap_blocks,
      .inode_blocks_total = inode_blocks,
      .data_block_start = 1 + inode_bitmap_blocks + block_bitmap_blocks +
                          rec_bitmap_blocks + inode_blocks,
      .root_inode = 1};

  write_superblock(file, &sb);

  // Mark the block bitmap with initialized blocks
  for (uint32_t i = 1; i <= total_blocks - data_blocks; i++) {
    bitmap_bit_toggle(file, sb.block_bitmap_start,
                      sb.block_bitmap_start + sb.block_bitmap_total, i);
  }

  create_root_directory(file, &sb);

  bitmap_bit_toggle(file, sb.inode_bitmap_start,
                    sb.inode_bitmap_start + sb.inode_bitmap_total,
                    sb.root_inode);
  bitmap_bit_toggle(file, sb.block_bitmap_start,
                    sb.block_bitmap_start + sb.block_bitmap_total,
                    sb.data_block_start);

  // Close file partition
  fclose(file);

  // Messages
  printf("BSFS partition formatted successfully\n");
  printf("Partition size: %d\n", sb.fs_size);
  printf("Block size (bytes): %d\n", BLOCK_SIZE);
  printf("Total blocks: %d\n", sb.total_blocks);
  printf("Inode blocks: %d\n", sb.inode_blocks_total);
  printf("Bitmap blocks: %d\n", sb.block_bitmap_total);
  printf("Inode bitmap blocks: %d\n", sb.inode_bitmap_total);
  printf("Data block start: %d\n", sb.data_block_start);
  printf("Root Directory Inode: %d\n", sb.root_inode);
}
