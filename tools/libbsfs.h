/*
 * libbsfs, the userspace side of the on-disk format for mkfs.bsfs and recovery.bsfs.
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

// bounds every walk, a child pointer aimed at an ancestor would loop forever
#define MAX_TREE_DEPTH 64

struct u32_vec {
  uint32_t *items;
  size_t    count;
  size_t    cap;
};

int  vec_contains(const struct u32_vec *v, uint32_t value);
int  vec_push_unique(struct u32_vec *v, uint32_t value);
void vec_remove(struct u32_vec *v, uint32_t value);

/*
 * writable opens refuse a mounted target (O_EXCL for block devices, a loop backing check for images) and
 * take an flock. returns an fd or -1, having printed why.
 */
int bsfs_dev_open(const char *path, int writable);
int bsfs_dev_size(int fd, uint64_t *bytes);
int bsfs_dev_sync(int fd);
int bsfs_read_block(int fd, uint32_t block, void *buf);
int bsfs_write_block(int fd, uint32_t block, const void *buf);

/* reads block 1, runs bsfs_super_check() and makes sure the device holds what it declares */
int bsfs_load_super(int fd, bsfs_super_block *sb);

struct bitmap_state {
  bitmap_block_t *blocks;
  uint32_t       *disk_blocks;
  uint32_t        count;
};

/* all-free chain for a superblock that's about to be written, mkfs uses this instead of loading */
int  bitmap_create(const bsfs_super_block *sb, struct bitmap_state *bm);
int  bitmap_load(int fd, const bsfs_super_block *sb, struct bitmap_state *bm);
int  bitmap_write(int fd, const struct bitmap_state *bm);
int  bitmap_is_set(const struct bitmap_state *bm, uint32_t block);
int  bitmap_set(struct bitmap_state *bm, uint32_t block, int used);
void bitmap_free(struct bitmap_state *bm);

typedef uint32_t (*entry_key_fn)(const void *entry);
typedef int (*entry_cmp_fn)(const void *a, const void *b);

struct tree_desc {
  const char  *name;
  size_t       entry_size;
  int          min_degree;
  entry_key_fn key;
  entry_cmp_fn cmp;
};

struct tree_desc bsfs_inode_tree(void);
struct tree_desc bsfs_dir_tree(void);
struct tree_desc bsfs_rec_tree(void);
struct tree_desc bsfs_bspan_tree(void);

/*
 * bitmap NULL makes a read-only txn: tree_read() skips the allocated check and nothing may allocate.
 * allocated blocks are never published until txn_commit(), so they can be freed or reused freely.
 */
struct bsfs_txn {
  int                     fd;
  const bsfs_super_block *sb;
  struct bitmap_state    *bitmap;
  struct u32_vec          allocated;
  struct u32_vec          retired;
};

void txn_destroy(struct bsfs_txn *tx);

/* syncs nodes and bitmap, then publishes new_sb. 0 ok, -1 before publication, -2 if publication is uncertain */
int txn_commit(struct bsfs_txn *tx, const bsfs_super_block *new_sb);

struct disk_node {
  uint32_t     block;
  btree_node_t raw;
};

int       tree_read(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t block, struct disk_node **out);
uint8_t  *tree_entries(struct disk_node *node);
uint32_t *tree_children(struct disk_node *node, const struct tree_desc *d);

/* 1 found (copied to out if non-NULL), 0 not found, -1 on error */
int tree_search(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t root, const void *needle, void *out);

/* rejects a duplicate. *root is updated to the new private root */
int tree_insert(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry);

/* replaces the entry comparing equal to @entry */
int tree_update(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry);

/* 0 continues, >0 stops the walk cleanly, <0 fails it */
typedef int (*tree_visit_fn)(const void *entry, void *ctx);

/*
 * leaf entries in order, failing on a cycle or out of order entries. nodes, if non-NULL, gets every node
 * block and the caller frees nodes->items.
 */
int tree_walk(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t root, tree_visit_fn visit, void *ctx,
              struct u32_vec *nodes);

/* no btree delete here, rebuilds from the surviving leaf entries and retires every old node */
int tree_rebuild_without_key(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t old_root, uint32_t remove_key,
                             uint32_t *new_root);

#endif
