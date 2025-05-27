/* filesystem_bsfs.h
 * Header file with structs used by the file system
 * BSFS - Basic Solution File System
 * Structs: Superblock, Inode, Inode Bitmap, Block Bitmap, Recover Blocks Bitmap
 * */

#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAGIC_NUMBER 0x53465342 // BSFS in hex
#define BLOCK_SIZE 4096         // Bytes
#define MAX_FILENAME 255        // Bytes
#define INODE_SIZE 512          // Bytes
#define ADDRESSES_PER_BLOCK BLOCK_SIZE / sizeof(uint32_t)
#define BITMAP_BLOCK_BYTES (BLOCK_SIZE - sizeof(uint32_t))
#define INVALID_BLOCK 0xFFFFFFFF

// Definition of the superblock struct
typedef struct {
  uint32_t magic_number;
  uint32_t block_size;
  uint32_t fs_size;
  uint32_t total_blocks;
  // Bitmap blocks
  uint32_t block_bitmap_start;
  uint32_t block_bitmap_total;
  // Inode blocks
  uint32_t inode_root;
  uint32_t recovery_root;
  // Data block
  uint32_t data_block_start;
  // Number of the root inode
  uint32_t root_inode;
} superblock_t;

typedef struct {
  uint8_t bits[BITMAP_BLOCK_BYTES];
  uint32_t next_block;
} bitmap_block_t;

/*
 * Definição da estrutura  bspan_t que contém:
 * - file_blk: endereço lógico do bloco, em que ordem está o bloco no arquivo
 * - disk_blk: endereço físico do bloco, número do bloco no FS
 * - lenght: quantidade de blocos subsequentes que contém dados
 * - padding: 16 bits para acertar 12 bytes de tamanho
 * */

typedef struct {
  uint32_t file_blk;
  uint32_t disk_blk;
  uint16_t length;
  uint16_t padding;
} bspan_t;

/*
 * Definição do inode
 * 256 bytes
 * */
typedef struct {
  uint32_t inode_number;
  uint32_t file_size;
  uint8_t file_type;
  uint32_t block_count;
  // User ID and Group ID
  uint16_t uid;
  uint16_t gid;
  uint16_t permissions;
  // Hard links count
  uint16_t links_count;
  // Timestamps
  uint32_t created_at;
  uint32_t modified_at;
  uint32_t accessed_at;
  // Blocks
  bspan_t direct_blocks[16];
  uint32_t btree_root;
  // Padding
  uint8_t padding[34];
} inode_t;

typedef struct {
  uint32_t index;
} rec_entry_t;

// Directory Entry
typedef struct {
  uint32_t inode_number;
  uint16_t entry_size;
  uint8_t name_size;
  uint8_t file_type;
  char filename[MAX_FILENAME];
} dir_entry_t;

typedef struct {
  inode_t inode;
  uint32_t pos;
} file_t;

typedef struct {
  inode_t inode;
  uint32_t dir_index;
} dir_t;

#endif // FILESYSTEM_BSFS
