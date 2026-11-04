/**
 * @file bsfs_shared.h
 * @brief On-disk format, shared by the kernel module and the tools in tools/.
 *
 * Disk layout, in 4 KiB blocks:
 *
 * @code{.unparsed}
 *   0                  reserved, zeroed
 *   1                  bsfs_super_block
 *   2 ..               the bitmap chain, block_bitmap_total blocks
 *   data_block_start.. everything else: tree nodes and file data
 * @endcode
 *
 * Every tree node is one btree_node_t. Nothing in here is packed, so the module
 * and the tools agree on layout only because they are built for the same ABI.
 * Numbers are stored in host byte order.
 */
#ifndef BSFS_FORMAT_H
#define BSFS_FORMAT_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef uint16_t __u16;
typedef uint8_t  __u8;
#endif

#define BSFS_BLOCK_SIZE 4096     ///< block size in bytes, the only one supported
#define MAGIC_NUMBER 0x53465342  ///< "BSFS" read as little-endian bytes
#define MAX_FILENAME 255         ///< size of bsfs_dentry.filename, NUL included
#define INVALID_BLOCK 0xFFFFFFFF ///< no block. allocation failure, and the end of the bitmap chain
#define BSFS_ROOT_INO 1          ///< inode number of the root directory

/// bitmap bytes per chain block. the last 4 bytes hold next_block
#define BITMAP_BLOCK_BYTES (BSFS_BLOCK_SIZE - sizeof(__u32))
#define BITS_PER_BLOCK (BITMAP_BLOCK_BYTES * 8) ///< blocks tracked by one chain block, 32736

/**
 * @brief The superblock, stored at block 1.
 *
 * inode_root and recovery_root are the roots of the inode tree and the recovery
 * tree. Copy-on-write moves them on every mutation. 0 means the tree is empty,
 * which only the recovery tree can be.
 * root_inode is always BSFS_ROOT_INO.
 */
typedef struct bsfs_super_block {
  __u32 magic_number;       ///< MAGIC_NUMBER
  __u32 block_size;         ///< always BSFS_BLOCK_SIZE
  __u64 fs_size;            ///< total_blocks * BSFS_BLOCK_SIZE, in bytes
  __u32 total_blocks;       ///< every block on the device, metadata included
  __u32 block_bitmap_start; ///< first block of the bitmap chain, 2 as mkfs writes it
  __u32 block_bitmap_total; ///< length of the bitmap chain
  __u32 inode_root;         ///< root node of the inode tree
  __u32 recovery_root;      ///< root node of the recovery tree, 0 while empty
  __u32 data_block_start;   ///< first block the allocator may hand out
  __u32 root_inode;         ///< always BSFS_ROOT_INO
} bsfs_super_block;

/**
 * @brief One block of the bitmap chain.
 *
 * next_block links to the following chain block. load_bitmap() walks it but
 * refuses links outside [block_bitmap_start, block_bitmap_start +
 * block_bitmap_total).
 */
typedef struct bitmap_block_t {
  __u8  bits[BITMAP_BLOCK_BYTES]; ///< one bit per block, LSB-first, set means allocated
  __u32 next_block;               ///< next chain block, INVALID_BLOCK after the last one
} bitmap_block_t;

/**
 * @brief An extent. Maps @p length file blocks from @p file_blk onto contiguous
 *        disk blocks from @p disk_blk.
 *
 * The entries of a file's extent tree, keyed by file_blk.
 */
typedef struct bspan_t {
  __u32 file_blk; ///< first file block of the run, the tree key
  __u32 disk_blk; ///< disk block file_blk maps to
  __u16 length;   ///< blocks in the run, at most BSFS_MAX_SPAN_BLOCKS
  __u16 padding;  ///< unused
} bspan_t;

/// length is a __u16, stop growing a span short of the wrap
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
  __u32 ino;         ///< inode number, the tree key
  __u32 size;        ///< file size in bytes
  __u8  unused;      ///< not maintained
  __u32 block_count; ///< not maintained
  __u16 uid;         ///< owner
  __u16 gid;         ///< group
  __u16 mode;        ///< the whole i_mode, type and permission bits
  __u16 nlinks;      ///< link count. an entry at 0 is a deleted inode, iget refuses it
  __u32 atime;       ///< last access
  __u32 mtime;       ///< last data change
  __u32 ctime;       ///< last inode change
  __u32 btree_root;  ///< extent tree or dentry tree, 0 while empty
  __u8  pad[34];     ///< zero on new inodes, carried over by updates
} disk_mapped_inode_t;

/**
 * @brief A directory entry, as stored in a directory's tree.
 *
 * Ordered by (hash_filename(filename), filename), so a hash collision still
 * sorts. name_hash stores that hash. compare_dir_entry() recomputes it instead
 * of trusting the stored copy.
 */
typedef struct bsfs_dentry {
  __u32 ino;                    ///< inode the name points at
  __u32 name_hash;              ///< hash_filename(filename), written but never trusted
  char  filename[MAX_FILENAME]; ///< NUL-terminated name
} bsfs_dentry;

/**
 * @brief A recovery snapshot of an unlinked file, keyed by recovery_id.
 *
 * The snapshot owns the tree at btree_root until it is restored or purged.
 * file_type is BSFS_REC_TYPE_DIR or BSFS_REC_TYPE_FILE, and permissions
 * holds the whole i_mode. deleted_at and retention_until are seconds since the
 * epoch. Purging drops expired entries first, then the oldest.
 */
typedef struct rec_entry_t {
  __u32 recovery_id;         ///< tree key, picked by bsfs_rec_tree_add()
  __u64 deleted_at;          ///< when evict recorded the snapshot
  __u32 original_parent_ino; ///< directory the last link was removed from
  char  original_name[256];  ///< name it had there
  __u32 inode_number;        ///< inode number it had. restore reuses it when free
  __u8  file_type;           ///< BSFS_REC_TYPE_DIR or BSFS_REC_TYPE_FILE
  __u16 uid;                 ///< owner at deletion
  __u16 gid;                 ///< group at deletion
  __u16 permissions;         ///< the whole i_mode
  __u32 file_size;           ///< size in bytes at deletion
  __u32 block_count;         ///< always 0, reclaim walks btree_root. kept for the layout
  __u32 btree_root;          ///< the file's extent tree, or dentry tree for a directory
  __u64 retention_until;     ///< expiry, deleted_at + DEFAULT_RECOVERY_TTL
} rec_entry_t;

#define BSFS_REC_TYPE_DIR 0  ///< rec_entry_t.file_type of a directory
#define BSFS_REC_TYPE_FILE 1 ///< anything that isn't a directory

/**
 * @brief One tree node, exactly one block.
 *
 * data holds up to 2t-1 entries back to back, then 2t __u32 child block
 * numbers. The minimum degree t depends on the entry size (see
 * bsfs_min_degree()), so index it through bsfs_node_children(), or bentries()
 * and bchildren() in btree.h.
 */
typedef struct btree_node_t {
  __u32 num_entries; ///< entries in use, at most 2t-1
  __u8  is_leaf;     ///< 1 for a leaf, 0 for an internal node

  /// must match the size btree_create_handle() assumes when it computes min_degree
  __u8 data[BSFS_BLOCK_SIZE - sizeof(__u32) - sizeof(__u8)];
} btree_node_t;

// host order and unpacked, so kernel and tools only agree while both lay these out the way x86_64 does
_Static_assert(sizeof(bsfs_super_block) == 48, "bsfs_super_block layout changed");
_Static_assert(sizeof(bitmap_block_t) == BSFS_BLOCK_SIZE, "bitmap_block_t must be exactly one block");
_Static_assert(sizeof(bspan_t) == 12, "bspan_t layout changed");
_Static_assert(sizeof(disk_mapped_inode_t) == 76, "disk_mapped_inode_t layout changed");
_Static_assert(offsetof(disk_mapped_inode_t, block_count) == 12, "disk_mapped_inode_t padding changed");
_Static_assert(sizeof(bsfs_dentry) == 264, "bsfs_dentry layout changed");
_Static_assert(sizeof(rec_entry_t) == 312, "rec_entry_t layout changed");
_Static_assert(offsetof(rec_entry_t, deleted_at) == 8, "rec_entry_t padding changed, i386 aligns __u64 to 4");
_Static_assert(sizeof(btree_node_t) == BSFS_BLOCK_SIZE, "btree_node_t must be exactly one block");

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

/**
 * @brief Largest minimum degree t whose node fits one block, i.e.
 *        entry_size * (2t - 1) + 4 * 2t + 5 <= BSFS_BLOCK_SIZE.
 *
 * Decides where bsfs_node_children() starts, so every reader of a tree has to
 * get the same answer for the same entry size.
 */
static inline int bsfs_min_degree(__u64 entry_size) {
  int t = 2;

  while (1) {
    __u64 entry_area    = entry_size * (2 * t - 1);
    __u64 pointer_area  = sizeof(__u32) * (2 * t);
    __u64 node_overhead = sizeof(__u8) + sizeof(__u32);

    if (entry_area + pointer_area + node_overhead > BSFS_BLOCK_SIZE)
      return t - 1;

    t++;
  }
}

/**
 * @brief A node's 2t child block numbers.
 *
 * They sit after the full entry area, not after num_entries, so the offset only
 * depends on t and the entry size.
 */
static inline __u32 *bsfs_node_children(btree_node_t *node, int t, size_t entry_size) {
  return (__u32 *)(node->data + (size_t)(2 * t - 1) * entry_size);
}

/** @brief Inode tree key: the inode number. */
static inline __u32 get_inode_key(const void *entry) {
  return ((const disk_mapped_inode_t *)entry)->ino;
}

/** @brief Orders inode entries by ino. */
static inline int compare_inode(const void *a, const void *b) {
  return (get_inode_key(a) > get_inode_key(b)) - (get_inode_key(a) < get_inode_key(b));
}

/** @brief Extent tree key: the first file block of the span. */
static inline __u32 get_bspan_key(const void *entry) {
  return ((const bspan_t *)entry)->file_blk;
}

/** @brief Orders extents by file_blk. */
static inline int compare_bspan(const void *a, const void *b) {
  return (get_bspan_key(a) > get_bspan_key(b)) - (get_bspan_key(a) < get_bspan_key(b));
}

/**
 * @brief Directory tree key: hash_filename() of the name.
 *
 * Not unique. Recomputed rather than read from name_hash so a stale stored hash
 * can't send a lookup down the wrong subtree.
 */
static inline __u32 get_dir_entry_key(const void *entry) {
  return hash_filename(((const bsfs_dentry *)entry)->filename);
}

/** @brief Orders dentries by name hash, then by name. */
static inline int compare_dir_entry(const void *a, const void *b) {
  __u32 ha = get_dir_entry_key(a);
  __u32 hb = get_dir_entry_key(b);

  if (ha != hb)
    return (ha > hb) - (ha < hb);
  return strcmp(((const bsfs_dentry *)a)->filename, ((const bsfs_dentry *)b)->filename);
}

/** @brief Recovery tree key: the snapshot's recovery_id. */
static inline __u32 get_rec_entry_key(const void *entry) {
  return ((const rec_entry_t *)entry)->recovery_id;
}

/** @brief Orders snapshots by recovery_id. */
static inline int compare_rec_entry(const void *a, const void *b) {
  return (get_rec_entry_key(a) > get_rec_entry_key(b)) - (get_rec_entry_key(a) < get_rec_entry_key(b));
}

/**
 * @brief Checks superblock geometry.
 *
 * Shared by mount, mkfs (on what it is about to write) and recovery.bsfs, so
 * they can't disagree on what a sane image is. Only looks at the superblock,
 * not at whether the device is big enough to hold it.
 *
 * @return NULL if sane, else a static string naming the first bad field.
 */
static inline const char *bsfs_super_check(const bsfs_super_block *sb) {
  __u64 bitmap_end = (__u64)sb->block_bitmap_start + sb->block_bitmap_total;

  if (sb->magic_number != MAGIC_NUMBER)
    return "bad magic";
  if (sb->block_size != BSFS_BLOCK_SIZE)
    return "block_size is not 4096";
  if (sb->fs_size != (__u64)sb->total_blocks * BSFS_BLOCK_SIZE)
    return "fs_size does not match total_blocks";
  if (sb->block_bitmap_start < 2)
    return "bitmap overlaps the superblock";
  if (sb->block_bitmap_total == 0 ||
      sb->block_bitmap_total < ((__u64)sb->total_blocks + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK)
    return "too few bitmap blocks for total_blocks";
  if (bitmap_end > sb->data_block_start)
    return "bitmap runs into the data region";
  if (sb->data_block_start >= sb->total_blocks)
    return "no data region";
  if (sb->inode_root < sb->data_block_start || sb->inode_root >= sb->total_blocks)
    return "inode_root outside the data region";
  if (sb->recovery_root && (sb->recovery_root < sb->data_block_start || sb->recovery_root >= sb->total_blocks))
    return "recovery_root outside the data region";
  if (sb->root_inode != BSFS_ROOT_INO)
    return "root_inode is not BSFS_ROOT_INO";
  return NULL;
}

#endif
