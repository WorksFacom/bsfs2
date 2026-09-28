/**
 * @file btree.h
 * @brief The on-disk B+tree behind every index in bsfs.
 *
 * One implementation, four kinds of tree: inodes, a file's extents (bspan_t), a
 * directory's entries and the recovery snapshots. A btree_handle_t says which.
 * Entries live in the leaves. Internal nodes hold copies of leaf entries as
 * separators, and a key equal to a separator lives in its right subtree.
 *
 * Every mutation is copy-on-write. It clones the root-to-leaf path it touches,
 * publishes the new root in handle->root_block, and hands the replaced blocks
 * to bsfs_defer_free_block(). The tree never writes its root anywhere else, so
 * the caller copies handle->root_block back into whatever owns the tree: the
 * inode's btree_root, or inode_root / recovery_root in the superblock.
 *
 * Nothing here takes a lock. The caller serializes mutations of one tree.
 * Node blocks come from allocate_meta_block() at handle->prio.
 */
#ifndef BTREE_BSFS_H
#define BTREE_BSFS_H

#include "../bsfs.h"
#include "alloc.h"
#include <linux/fs.h>
#include <linux/types.h>

/**
 * @brief Allocates a handle for one kind of tree. Free it with kfree().
 *
 * The handle starts empty: root_block 0, prio BSFS_ALLOC_GROW. Callers point
 * root_block at an existing tree and lower prio for paths that shrink it.
 * min_degree t is the largest that fits a node in one block, i.e.
 * entry_size * (2t - 1) + 4 * 2t + 5 <= BSFS_BLOCK_SIZE.
 *
 * @param cmp orders whole entries. NULL compares the first __u32 of each.
 * @param get_key extracts the numeric key btree_search() and btree_remove() use.
 * @return the handle, or NULL on OOM.
 */
btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size, btree_compare_fn cmp, btree_key_fn get_key);

/** @brief Inode tree key: the inode number. */
static inline __u32 get_inode_key(const void *entry) {
  return ((struct disk_mapped_inode_t *)entry)->ino;
}

/** @brief Orders inode entries by ino. */
static inline int compare_inode(const void *a, const void *b) {
  return (get_inode_key(a) > get_inode_key(b)) - (get_inode_key(a) < get_inode_key(b));
}

/** @brief Handle for the inode tree, holding disk_mapped_inode_t. NULL on OOM. */
static inline btree_handle_t *btree_create_inode_handle(void) {
  return btree_create_handle(BTREE_TYPE_INODE, sizeof(struct disk_mapped_inode_t), compare_inode, get_inode_key);
}

/** @brief Extent tree key: the first file block of the span. */
static inline __u32 get_bspan_key(const void *entry) {
  return ((const bspan_t *)entry)->file_blk;
}

/** @brief Orders extents by file_blk. */
static inline int compare_bspan(const void *a, const void *b) {
  return (get_bspan_key(a) > get_bspan_key(b)) - (get_bspan_key(a) < get_bspan_key(b));
}

/** @brief Handle for a file's extent tree, holding bspan_t. NULL on OOM. */
static inline btree_handle_t *btree_create_bspan_handle(void) {
  return btree_create_handle(BTREE_TYPE_BSPAN, sizeof(bspan_t), compare_bspan, get_bspan_key);
}

/** @brief Start of a node's entries. Room for 2t - 1 of them. */
static inline __u8 *bentries(struct btree_node *node) {
  return node->node->data;
}

/**
 * @brief Start of a node's child block numbers, 2t of them.
 *
 * They sit after the full entry area, not after num_entries, so the offset only
 * depends on the handle.
 */
static inline __u32 *bchildren(struct btree_node *node, btree_handle_t *handle) {

  size_t children_start = (2 * handle->min_degree - 1) * handle->entry_size;
  return (__u32 *)(node->node->data + children_start);
}

/**
 * @brief Directory tree key: hash_filename() of the name.
 *
 * Not unique. Look entries up with btree_search_entry(), which compares names
 * too.
 */
static inline __u32 get_dir_entry_key(const void *entry) {
  return hash_filename(((const struct bsfs_dentry *)entry)->filename);
}

/** @brief Orders dentries by name hash, then by name. */
static inline int compare_dir_entry(const void *a, const void *b) {
  __u32 ha = get_dir_entry_key(a);
  __u32 hb = get_dir_entry_key(b);

  if (ha < hb)
    return -1;
  if (ha > hb)
    return 1;

  const struct bsfs_dentry *ea = (const struct bsfs_dentry *)a;
  const struct bsfs_dentry *eb = (const struct bsfs_dentry *)b;
  return strcmp(ea->filename, eb->filename);
}

/** @brief Handle for a directory's entry tree, holding bsfs_dentry. NULL on OOM. */
static inline btree_handle_t *btree_create_dir_entry_handle(void) {
  return btree_create_handle(BTREE_TYPE_DIR_ENTRY, sizeof(struct bsfs_dentry), compare_dir_entry, get_dir_entry_key);
}

/** @brief Recovery tree key: the snapshot's recovery_id. */
static inline __u32 get_rec_entry_key(const void *entry) {
  return ((const rec_entry_t *)entry)->recovery_id;
}

/** @brief Orders snapshots by recovery_id. */
static inline int compare_rec_entry(const void *a, const void *b) {
  return (get_rec_entry_key(a) > get_rec_entry_key(b)) - (get_rec_entry_key(a) < get_rec_entry_key(b));
}

/** @brief Handle for the recovery tree, holding rec_entry_t. NULL on OOM. */
static inline btree_handle_t *btree_create_rec_entry_handle(void) {
  return btree_create_handle(BTREE_TYPE_RECOVERY, sizeof(rec_entry_t), compare_rec_entry, get_rec_entry_key);
}

/**
 * @brief Inserts a copy of @p entry into the tree at handle->root_block.
 *
 * Does not check for duplicates. Callers that need unique keys look first. On
 * failure the shadow blocks are freed and the live tree is untouched.
 *
 * @return 0, or -1 for any failure, including ENOSPC, ENOMEM and EIO.
 */
int btree_insert(struct super_block *sb, btree_handle_t *handle, void *entry);

/**
 * @brief Finds the entry with @p key, without copying it.
 *
 * On a hit, *output points into the leaf's buffer and *out_node is that leaf.
 * The caller owns it and must btree_free_node() it when done. Do not keep the
 * pointer across a mutation of the same tree: copy-on-write clones the leaf
 * and releases the block it came from.
 *
 * @param node_block where to start, normally handle->root_block.
 * @return 1 if found, 0 if not, or a negative error from btree_read_node().
 */
int btree_search(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, void **output, struct btree_node **out_node);

/**
 * @brief Finds the entry equal to @p needle under handle->cmp and copies it out.
 *
 * For trees whose key is not unique, like directories, where two names can
 * share a hash. Leaves nothing pinned.
 *
 * @param needle an entry of the tree's type, filled in as far as cmp looks.
 * @param output receives a copy of the match, entry_size bytes.
 * @return 1 if found, 0 if not, or a negative read error.
 */
int btree_search_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle, void *output);

/**
 * @brief Finds the extent with the greatest file_blk <= @p key.
 *
 * That is the only span that can contain file block @p key, so bsfs_get_block()
 * checks the length itself. Starts at handle->root_block.
 *
 * @return 1 with a copy in *out, 0 if no span starts at or before @p key, or a
 *         negative read error. *out is only written on 1.
 */
int bspan_search_le(struct super_block *sb, btree_handle_t *handle, uint32_t key, bspan_t *out);

/**
 * @brief Replaces the entry whose key matches @p new_entry's.
 *
 * Still copy-on-write, so a successful update always moves handle->root_block.
 *
 * @param node_block ignored. The search always starts at handle->root_block.
 * @return 1 if updated. 0 if the key is missing or anything failed, which the
 *         caller cannot tell apart.
 */
int btree_update(struct super_block *sb, btree_handle_t *handle, __u32 node_block, void *new_entry);

/**
 * @brief Removes the entry with @p key.
 *
 * Removing the last entry leaves handle->root_block at 0. Removal allocates
 * too, so shrink paths set handle->prio to BSFS_ALLOC_SHRINK first.
 *
 * @return 1 if removed. 0 if the key is missing or anything failed.
 */
int btree_remove(struct super_block *sb, btree_handle_t *handle, __u32 key);

/**
 * @brief Removes the entry equal to @p entry under handle->cmp.
 *
 * The directory counterpart of btree_remove(). Copies @p entry first, so it may
 * point into a node of this same tree.
 *
 * @return 1 if removed. 0 if not found or anything failed.
 */
int btree_remove_entry(struct super_block *sb, btree_handle_t *handle, const void *entry);

/**
 * @brief Reads node @p block and checks its header.
 *
 * *out_node wraps the buffer head, so node->node points straight into the
 * block. Release it with btree_free_node(). *out_node is NULL on failure.
 *
 * @return 0, -EUCLEAN for block 0, INVALID_BLOCK, a block past the end or a
 *         corrupt header, -EIO, or -ENOMEM.
 */
int btree_read_node(struct super_block *sb, btree_handle_t *handle, __u32 block, struct btree_node **out_node);

/** @brief Releases a node from btree_read_node() or btree_search(): brelse, then kfree. */
void btree_free_node(struct btree_node *node);

/**
 * @brief Queues every block of the tree at @p node_block for release.
 *
 * Node blocks always, and for BTREE_TYPE_BSPAN the data blocks of every span
 * too. Nothing is freed until the next bsfs_commit_deferred_frees(). Spans that
 * point past the end of the filesystem are skipped with a warning. Leaves
 * handle->root_block alone. The caller drops its reference to the tree.
 *
 * @param blocks_deferred if not NULL, incremented by the number of blocks queued.
 * @return 0, or -1 on a read error or OOM, with part of the tree already queued.
 */
int btree_deallocate_all(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u64 *blocks_deferred);

#endif // BTREE_BSFS_H
