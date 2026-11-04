/**
 * @file libbsfs.h
 * @brief libbsfs, the userspace side of the on-disk format for mkfs.bsfs and recovery.bsfs.
 *
 * the format itself (structs, key/cmp, min degree, superblock check) lives in include/bsfs_shared.h and is
 * compiled into the kernel too. this adds what only the tools need: raw block io on an fd, an in-memory
 * bitmap, and a copy-on-write b+ tree that allocates from that bitmap inside a txn and publishes by
 * rewriting the superblock last.
 */
#ifndef BSFS_LIBBSFS_H
#define BSFS_LIBBSFS_H

#include "../include/bsfs_shared.h"
#include <stddef.h>
#include <stdint.h>

/// bounds every walk, a child pointer aimed at an ancestor would loop forever
#define MAX_TREE_DEPTH 64

/** @brief growable array of block numbers, used as a set. zero-init is empty, free items when done. */
struct u32_vec {
  uint32_t *items; ///< malloc'd, unordered
  size_t    count; ///< entries in use
  size_t    cap;   ///< room before the next realloc
};

/** @brief 1 if @p value is in @p v, else 0. linear. */
int vec_contains(const struct u32_vec *v, uint32_t value);
/** @brief appends @p value unless it is already there. 0, or -1 on OOM. */
int vec_push_unique(struct u32_vec *v, uint32_t value);
/** @brief drops @p value if present, moving the last entry into its slot. */
void vec_remove(struct u32_vec *v, uint32_t value);

/**
 * @brief opens an image or block device.
 *
 * writable opens refuse a mounted target (O_EXCL for block devices, a loop backing check for images) and
 * take an flock. returns an fd or -1, having printed why.
 */
int bsfs_dev_open(const char *path, int writable);
/** @brief size of a regular file or block device in bytes. 0, or -1 for anything else. */
int bsfs_dev_size(int fd, uint64_t *bytes);
/** @brief fsync(2). every commit step goes through it. */
int bsfs_dev_sync(int fd);
/** @brief reads one BSFS_BLOCK_SIZE block into @p buf. 0, or -1 on an error or short read. */
int bsfs_read_block(int fd, uint32_t block, void *buf);
/** @brief writes one BSFS_BLOCK_SIZE block from @p buf. 0, or -1 on an error or short write. */
int bsfs_write_block(int fd, uint32_t block, const void *buf);

/** reads block 1, runs bsfs_super_check() and makes sure the device holds what it declares */
int bsfs_load_super(int fd, bsfs_super_block *sb);

/** @brief the whole bitmap chain in memory. nothing reaches the disk until bitmap_write(). */
struct bitmap_state {
  bitmap_block_t *blocks;      ///< chain blocks in chain order
  uint32_t       *disk_blocks; ///< where each of blocks lives on disk
  uint32_t        count;       ///< chain length, block_bitmap_total
};

/** all-free chain for a superblock that's about to be written, mkfs uses this instead of loading */
int bitmap_create(const bsfs_super_block *sb, struct bitmap_state *bm);
/**
 * @brief reads the chain from block_bitmap_start.
 *
 * same link rule as the kernel's load_bitmap(), and also refuses a block visited twice or a chain that
 * doesn't end in INVALID_BLOCK. 0, or -1 having printed why.
 */
int bitmap_load(int fd, const bsfs_super_block *sb, struct bitmap_state *bm);
/** @brief writes every chain block back, no sync. 0 or -1. */
int bitmap_write(int fd, const struct bitmap_state *bm);
/** @brief 1 if @p block is allocated. 0 if free or past the end of the chain. */
int bitmap_is_set(const struct bitmap_state *bm, uint32_t block);
/** @brief marks @p block allocated (@p used non-zero) or free. -1 if past the end of the chain. */
int bitmap_set(struct bitmap_state *bm, uint32_t block, int used);
/** @brief frees the arrays and zeroes @p bm. safe on a zeroed or failed state. */
void bitmap_free(struct bitmap_state *bm);

/** @brief numeric key of an entry, like btree_key_fn in the kernel. */
typedef uint32_t (*entry_key_fn)(const void *entry);
/** @brief orders two whole entries, like btree_compare_fn in the kernel. */
typedef int (*entry_cmp_fn)(const void *a, const void *b);

/** @brief one kind of tree, the userspace btree_handle_t minus the root. */
struct tree_desc {
  const char  *name;       ///< for error messages
  size_t       entry_size; ///< bytes per entry
  int          min_degree; ///< t, from bsfs_min_degree()
  entry_key_fn key;        ///< numeric key, only tree_rebuild_without_key() uses it
  entry_cmp_fn cmp;        ///< orders entries, every search and insert goes by it
};

/** @brief descriptor of the inode tree. */
struct tree_desc bsfs_inode_tree(void);
/** @brief descriptor of a directory tree. */
struct tree_desc bsfs_dir_tree(void);
/** @brief descriptor of the recovery tree. */
struct tree_desc bsfs_rec_tree(void);
/** @brief descriptor of a file's extent tree. */
struct tree_desc bsfs_bspan_tree(void);

/**
 * @brief one copy-on-write transaction over an open image.
 *
 * bitmap NULL makes a read-only txn: tree_read() skips the allocated check and nothing may allocate.
 * allocated blocks are never published until txn_commit(), so they can be freed or reused freely.
 */
struct bsfs_txn {
  int                     fd;        ///< from bsfs_dev_open()
  const bsfs_super_block *sb;        ///< geometry the txn checks every block against
  struct bitmap_state    *bitmap;    ///< allocates from here, NULL for read-only
  struct u32_vec          allocated; ///< blocks this txn allocated, private until commit
  struct u32_vec          retired;   ///< published blocks it replaced, freed after the commit
};

/** @brief frees the txn's lists. the bitmap belongs to the caller. */
void txn_destroy(struct bsfs_txn *tx);

/** syncs nodes and bitmap, then publishes new_sb. 0 ok, -1 before publication, -2 if publication is uncertain */
int txn_commit(struct bsfs_txn *tx, const bsfs_super_block *new_sb);

/** @brief a tree node copied into memory. free() it, there is nothing pinned. */
struct disk_node {
  uint32_t     block; ///< where it was read from or will be written
  btree_node_t raw;   ///< the block's contents
};

/**
 * @brief reads and checks one node.
 *
 * refuses blocks outside the data region, blocks the bitmap calls free when there is one, and corrupt
 * headers. 0 with a malloc'd node in *out, or -1 having printed why.
 */
int tree_read(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t block, struct disk_node **out);
/** @brief start of a node's entries. */
uint8_t *tree_entries(struct disk_node *node);
/** @brief start of a node's 2t child block numbers, see bsfs_node_children(). */
uint32_t *tree_children(struct disk_node *node, const struct tree_desc *d);

/** 1 found (copied to out if non-NULL), 0 not found, -1 on error */
int tree_search(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t root, const void *needle, void *out);

/** rejects a duplicate. *root is updated to the new private root */
int tree_insert(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry);

/** replaces the entry comparing equal to @p entry */
int tree_update(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry);

/** 0 continues, >0 stops the walk cleanly, <0 fails it */
typedef int (*tree_visit_fn)(const void *entry, void *ctx);

/**
 * leaf entries in order, failing on a cycle or out of order entries. nodes, if non-NULL, gets every node
 * block and the caller frees nodes->items.
 */
int tree_walk(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t root, tree_visit_fn visit, void *ctx,
              struct u32_vec *nodes);

/** no btree delete here, rebuilds from the surviving leaf entries and retires every old node */
int tree_rebuild_without_key(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t old_root, uint32_t remove_key,
                             uint32_t *new_root);

#endif
