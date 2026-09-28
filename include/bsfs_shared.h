/**
 * @file bsfs_shared.h
 * @brief On-disk format, shared by the kernel module and the tools in tools/.
 *
 * Disk layout, in 4 KiB blocks:
 *
 *   0                  reserved, zeroed
 *   1                  bsfs_super_block
 *   2 ..               the bitmap chain, block_bitmap_total blocks
 *   data_block_start.. everything else: tree nodes and file data
 *
 * Every tree node is one btree_node_t. Nothing in here is packed, so the module
 * and the tools agree on layout only because they are built for the same ABI.
 * Numbers are stored in host byte order.
 */
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
#define MAGIC_NUMBER 0x53465342 // "BSFS" read as little-endian bytes
#define MAX_FILENAME 255        // size of bsfs_dentry.filename, NUL included
#define INVALID_BLOCK 0xFFFFFFFF
#define BSFS_ROOT_INO 1

// bitmap bytes per chain block. the last 4 bytes hold next_block
#define BITMAP_BLOCK_BYTES (BSFS_BLOCK_SIZE - sizeof(__u32))
#define BITS_PER_BLOCK (BITMAP_BLOCK_BYTES * 8)

/**
 * @brief The superblock, stored at block 1.
 *
 * inode_root and recovery_root are the roots of the inode tree and the recovery
 * tree. Copy-on-write moves them on every mutation. 0 means the tree is empty.
 * root_inode is always BSFS_ROOT_INO.
 */
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

/**
 * @brief One block of the bitmap chain.
 *
 * next_block links to the following chain block. load_bitmap() walks it but
 * refuses links outside [block_bitmap_start, block_bitmap_start +
 * block_bitmap_total).
 */
typedef struct bitmap_block_t {
  __u8  bits[BITMAP_BLOCK_BYTES];
  __u32 next_block;
} bitmap_block_t;

/**
 * @brief An extent. Maps @p length file blocks from @p file_blk onto contiguous
 *        disk blocks from @p disk_blk.
 *
 * The entries of a file's extent tree, keyed by file_blk.
 */
typedef struct bspan_t {
  __u32 file_blk;
  __u32 disk_blk;
  __u16 length;
  __u16 padding;
} bspan_t;

// length is a __u16, stop growing a span short of the wrap
#define BSFS_MAX_SPAN_BLOCKS 0xF000

/**
 * @brief An inode as stored in the inode tree, keyed by ino.
 *
 * btree_root is the file's extent tree for regular files and symlinks, and its
 * dentry tree for directories. 0 means empty. Times are seconds since the
 * epoch. mode carries the S_IF* type bits. block_count and unused are not
 * maintained.
 */
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

/**
 * @brief A directory entry, as stored in a directory's tree.
 *
 * Ordered by (hash_filename(filename), filename), so a hash collision still
 * sorts. name_hash stores that hash. compare_dir_entry() recomputes it instead
 * of trusting the stored copy.
 */
typedef struct bsfs_dentry {
  __u32 ino;
  __u32 name_hash;
  char  filename[MAX_FILENAME];
} bsfs_dentry;

/**
 * @brief A recovery snapshot of an unlinked file, keyed by recovery_id.
 *
 * The snapshot owns the tree at btree_root until it is restored or purged.
 * file_type is 0 for a directory and 1 for anything else, and permissions
 * holds the whole i_mode. deleted_at and retention_until are seconds since the
 * epoch. Purging drops expired entries first, then the oldest.
 */
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
  __u32 block_count; // always 0, reclaim walks btree_root. kept for the layout
  __u32 btree_root;
  __u64 retention_until;
} rec_entry_t;

/**
 * @brief One tree node, exactly one block.
 *
 * data holds up to 2t-1 entries back to back, then 2t __u32 child block
 * numbers. The minimum degree t depends on the entry size (see
 * btree_create_handle()), so index it through bentries() and bchildren() in
 * btree.h.
 */
typedef struct btree_node_t {
  __u32 num_entries;
  __u8  is_leaf;

  // must match the size btree_create_handle() assumes when it computes min_degree
  __u8 data[BSFS_BLOCK_SIZE - sizeof(__u32) - sizeof(__u8)];
} btree_node_t;

/**
 * @brief djb2 over a NUL-terminated name. The primary key of a dentry.
 *
 * The value lands on disk as bsfs_dentry.name_hash and decides the order of
 * every directory tree, so changing it breaks existing images.
 */
static inline __u32 hash_filename(const char *name) {
  int c;

  __u32 hash = 5381;

  while ((c = *name++))
    hash = ((hash << 5) + hash) + (unsigned char)c;

  return hash;
}

#endif
