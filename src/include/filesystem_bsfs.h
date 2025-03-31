/* filesystem_bsfs.h
 * Header file with structs used by the file system
 * BSFS - Basic Solution File System
 * Structs: Superblock, Inode, Inode Bitmap, Block Bitmap, Recover Blocks Bitmap
 * */

#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include <stdint.h>

#define MAGIC_NUMBER 0x53465342 // BSFS in hex
#define BLOCK_SIZE 4096         // Bytes
#define MAX_FILENAME 255        // Bytes
#define INODE_SIZE 512          // Bytes

// Definition of the superblock struct
typedef struct {
  uint32_t magic_number;
  uint32_t block_size;
  uint32_t fs_size;
  // Inode blocks and total blocks
  uint32_t total_inodes;
  uint32_t total_blocks;
  // Number of free inodes and free blocks
  uint32_t free_inodes;
  uint32_t free_blocks;
  // Bitmap blocks
  uint32_t inode_bitmap_start;
  uint32_t inode_bitmap_total;
  uint32_t block_bitmap_start;
  uint32_t block_bitmap_total;
  uint32_t rec_bitmap_start;
  uint32_t rec_bitmap_total;
  // Inode blocks
  uint32_t inode_blocks_start;
  uint32_t inode_blocks_total;
  // Data block
  uint32_t data_block_start;
  // Number of the root inode
  uint32_t root_inode;
} superblock_t;

// Definition of the inode struct
// Inode has 512 bytes of size
typedef struct {
  uint32_t file_size;
  uint16_t file_type;
  uint32_t block_count;
  // User ID and Group ID
  uint32_t uid;
  uint32_t gid;
  uint16_t permissions;
  // Hard links count
  uint16_t links_count;
  // Timestamps
  uint32_t created_at;
  uint32_t modified_at;
  uint32_t accessed_at;
  // Direct and indirect blocks
  uint32_t direct_blocks[16];
  uint32_t indirect_block[16];
  uint32_t double_indirect_block[16];
  // Padding
  uint8_t padding[30];
} inode_t;

// Bitmaps
typedef struct {
  uint8_t data[BLOCK_SIZE];
} block_bitmap_t;

typedef struct {
  uint8_t data[BLOCK_SIZE];
} recovery_blocks_bitmap_t;

typedef struct {
  uint8_t data[BLOCK_SIZE];
} inodes_bitmap_t;

// Directory Entry
typedef struct {
  uint32_t inode_number;
  uint16_t entry_size;
  uint8_t name_size;
  uint8_t file_type;
  char filename[MAX_FILENAME];
} directory_entry_t;

#endif // FILESYSTEM_BSFS
