#ifndef BSFS_FORMAT_H
#define BSFS_FORMAT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef uint16_t __u16;
typedef uint8_t  __u8;
#endif

#define BSFS_BLOCK_SIZE 4096
#define MAGIC_NUMBER 0x53465342
#define MAX_FILENAME 255
#define INVALID_BLOCK 0xFFFFFFFF
#define BSFS_ROOT_INO 1

#define BITMAP_BLOCK_BYTES (BSFS_BLOCK_SIZE - sizeof(__u32))
#define BITS_PER_BLOCK (BITMAP_BLOCK_BYTES * 8)

typedef struct bsfs_super_block {
  __u32 magic_number;
  __u32 block_size;
  __u64 fs_size;
  __u32 total_blocks;
  __u32 block_bitmap_start;
  __u32 block_bitmap_total;
  __u32 inode_root;
  __u32 recovery_root;
  __u32 data_block_start;
  __u32 root_inode;
} bsfs_super_block;

typedef struct bitmap_block_t {
  __u8  bits[BITMAP_BLOCK_BYTES];
  __u32 next_block;
} bitmap_block_t;

typedef struct bspan_t {
  __u32 file_blk;
  __u32 disk_blk;
  __u16 length;
  __u16 padding;
} bspan_t;

// length is a __u16; stop growing a span short of the wrap
#define BSFS_MAX_SPAN_BLOCKS 0xF000

typedef struct disk_mapped_inode_t {
  __u32 ino;
  __u32 size;
  __u8  unused;
  __u32 block_count;
  __u16 uid;
  __u16 gid;
  __u16 mode;
  __u16 nlinks;
  __u32 atime;
  __u32 mtime;
  __u32 ctime;
  __u32 btree_root;
  __u8  pad[34];
} disk_mapped_inode_t;

typedef struct bsfs_dentry {
  __u32 ino;
  __u32 name_hash;
  char  filename[MAX_FILENAME];
} bsfs_dentry;

typedef struct rec_entry_t {
  __u32 recovery_id;
  __u64 deleted_at;
  __u32 original_parent_ino;
  char  original_name[256];
  __u32 inode_number;
  __u8  file_type;
  __u16 uid;
  __u16 gid;
  __u16 permissions;
  __u32 file_size;
  __u32 block_count; // TODO: is this still useful in kernel?
  __u32 btree_root;
  __u64 retention_until;
} rec_entry_t;

typedef struct btree_node_t {
  __u32 num_entries;
  __u8  is_leaf;

  // data[BSFS_BLOCK_SIZE - 5 bytes from above]
  // stores packed entries followed by child pointers
  // kernel's compute_min_degree() must be consistent with this
  __u8 data[BSFS_BLOCK_SIZE - sizeof(__u32) - sizeof(__u8)];
} btree_node_t;

/**
 * @brief Computes the djb2 hash of a given filename.
 *
 * This function implements the classic djb2 hashing algorithm created by
 * Daniel J. Bernstein. It is fast and widely used for hashing strings.
 *
 * @param name The null-terminated string (filename) to be hashed.
 * @return A 32-bit unsigned integer hash value.
 */
static inline __u32 hash_filename(const char *name) {
  int c;

  __u32 hash = 5381;

  while ((c = *name++))
    hash = ((hash << 5) + hash) + (unsigned char)c;

  return hash;
}

#endif
