/**
 * @file rec_entry_operations_bsfs.h
 * @brief Recovery entry B-tree operations for BSFS.
 *
 * This header defines the public API to manage the Recovery B-tree used by
 * BSFS to track deleted files for later restoration. Each record stores a
 * snapshot of the file's inode metadata and block mapping at deletion time.
 *
 * Provided operations:
 *  - Initialization of the recovery tree in the filesystem image.
 *  - Add / remove / search recovery entries by key.
 *  - Purge (garbage collect) old entries by timestamp threshold.
 */

#ifndef REC_ENTRY_OPERATIONS_BSFS_H
#define REC_ENTRY_OPERATIONS_BSFS_H

#include "../bsfs.h"

/**
 * @brief Initialize (or load) the Recovery B-tree handle.
 *
 * If the recovery tree does not exist yet, this function creates a new B-tree
 * (root block allocation and on-disk initialization) and persists its root
 * into the superblock. If it already exists, this function creates an in-memory
 * handle pointing at the existing root.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (may be updated if the root is created).
 * @param out  Output: initialized B-tree handle for the recovery tree.
 * @return 1 on success, 0 on failure.
 */
void bsfs_rec_tree_mount_init(struct super_block *sb);

/**
 * @brief Insert a recovery entry into the Recovery B-tree.
 *
 * This function inserts a @ref rec_entry_t into the recovery tree. The
 * B-tree must be configured so that:
 *  - handle->entry_size == sizeof(rec_entry_t);
 *  - handle->get_key points to the recovery_id field inside rec_entry_t; and
 *  - handle->cmp compares recovery_id in ascending order (or matches your
 *    chosen ordering).
 *
 * If @p entry->recovery_id is 0, a new unique ID is assigned via
 * get_next_rec_id(). On duplicate key, the underlying B-tree insert is
 * expected to fail and this function returns 0.
 *
 * @param disk      Open filesystem image.
 * @param sb        Loaded superblock (used for node allocations).
 * @param rec_tree  Recovery B-tree handle (already initialized).
 * @param entry     In/out recovery entry to insert. If recovery_id == 0, it
 *                  will be set to a fresh ID before insertion.
 * @return 1 on success; 0 on failure (I/O error or duplicate key).
 */
int bsfs_rec_tree_add(struct super_block *sb, btree_handle_t *rec_tree, rec_entry_t *entry);

/**
 * @brief Remove a recovery entry from the Recovery B-tree by ID.
 *
 * This function deletes a single @ref rec_entry_t identified by its
 * @p recovery_id from the Recovery tree. The B-tree for recovery entries
 * must be configured so that the key is the 32-bit @c recovery_id field of
 * @ref rec_entry_t (i.e., handle->get_key returns &entry->recovery_id).
 *
 * Semantics:
 *  - If an entry with the given @p recovery_id exists, it is removed from
 *    the tree and the function returns 1.
 *  - If no such entry exists, the function returns 0.
 *
 * Notes:
 *  - This function only removes the metadata entry from the Recovery B-tree.
 *    Reclaiming data blocks referenced by the entry (if any) must be handled
 *    by a higher-level GC routine before or after removal, depending on your
 *    chosen policy.
 *
 * @param disk       Open filesystem image.
 * @param sb         Loaded superblock.
 * @param rec_tree   Recovery B-tree handle (already initialized).
 * @param recovery_id Unique identifier of the recovery entry to remove.
 * @return 1 if the entry was found and removed; 0 otherwise.
 */
int bsfs_rec_tree_remove(struct super_block *sb, btree_handle_t *rec_tree, __u32 recovery_id);

/**
 * @brief Drops recovery snapshots, expired or oldest first, and frees the
 *        blocks they were holding.
 *
 * Deleting a file parks its extent tree in a snapshot rather than releasing it,
 * so this is the only thing that ever gives that space back. Meant to run under
 * space pressure only - see bsfs_reclaim_space(). Takes sbi->rec_lock itself.
 *
 * @param blocks_deferred Optional count of snapshot blocks queued for release.
 * @return the number of entries purged.
 */
unsigned int bsfs_rec_tree_purge(struct super_block *sb, unsigned int max_entries, __u64 *blocks_deferred);

#endif /* REC_ENTRY_OPERATIONS_BSFS_H */
