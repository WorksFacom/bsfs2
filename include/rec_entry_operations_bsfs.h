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

#include "btree_bsfs.h"
#include "filesystem_bsfs.h"
#include "superblock_operations_bsfs.h"
#include <stdint.h>
#include <stdio.h>

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
int rec_tree_init(FILE *disk, superblock_t *sb, btree_handle_t *out);

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
int rec_tree_add(FILE *disk, superblock_t *sb, btree_handle_t *rec_tree,
                 rec_entry_t *entry);
/**
 * @brief Fetch the next available recovery_id in the Recovery tree.
 *
 * Scans the tree (if needed) to keep the monotonic recovery_id counter in sync
 * with the current root stored in @p sb. This mirrors get_next_inode_number.
 *
 * @param disk      Open filesystem image.
 * @param sb        Loaded superblock (provides the recovery root and caches).
 * @param rec_tree  Recovery B-tree handle used for traversals.
 * @return Next unused recovery_id.
 */
uint32_t get_next_rec_id(FILE *disk, superblock_t *sb,
                         btree_handle_t *rec_tree);

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
int rec_tree_remove(FILE *disk, superblock_t *sb, btree_handle_t *rec_tree,
                    uint32_t recovery_id);

/**
 * @brief Look up a recovery entry in the Recovery B-tree by ID.
 *
 * This function searches the Recovery B-tree for a @ref rec_entry_t whose
 * key is the 32-bit @p recovery_id. The B-tree must be configured so that
 * the key for each node record is the @c recovery_id field of @ref rec_entry_t
 * (i.e., the handle's key accessor returns &entry->recovery_id).
 *
 * Semantics:
 *  - If an entry with the given @p recovery_id exists, it is copied into
 *    @p out_entry and the function returns 1.
 *  - If not found, the function returns 0 and leaves @p out_entry unmodified.
 *
 * @param disk        Open filesystem image.
 * @param rec_tree    Recovery B-tree handle (already initialized).
 * @param recovery_id Key (unique ID) of the recovery entry to find.
 * @param out_entry   Output buffer to receive the found entry (must be
 * non-NULL).
 * @return 1 if found; 0 if not found or on invalid arguments.
 */
int rec_tree_search(FILE *disk, btree_handle_t *rec_tree, uint32_t recovery_id,
                    rec_entry_t *out_entry);

/**
 * @brief Find recovery entries by original filename.
 *
 * Performs an in-order traversal over the Recovery B-tree (keyed by
 * rec_entry_t::recovery_id) and collects entries whose
 * rec_entry_t::original_name matches exactly the provided @p name.
 *
 * Up to @p max matches are copied into the @p out buffer (if non-NULL). The
 * total number of matches found in the tree (which can be greater than @p max)
 * is returned via @p out_count.
 *
 * @param disk       Open filesystem image.
 * @param rec_tree   Initialized Recovery B-tree handle.
 * @param name       Exact filename to match against rec_entry_t::original_name.
 * @param out        Output buffer for matches (may be NULL to only count).
 * @param max        Capacity of @p out in number of entries.
 * @param out_count  Output: total number of matches found.
 * @return 1 on success (including zero matches), 0 on invalid args or I/O
 *         failure while traversing.
 */
int rec_tree_find_by_name(FILE *disk, btree_handle_t *rec_tree,
                          const char *name, rec_entry_t *out, size_t max,
                          size_t *out_count);

/**
 * @brief Permanently purge a deleted file from the filesystem.
 *
 * Reclaims all data blocks referenced by the snapshot @p rec (both direct
 * spans and spans in the per-file data B-tree), removes the span records, and
 * finally removes the metadata entry from the Recovery B-tree.
 *
 * Failure semantics:
 *  - If deallocation of data or removal of span mappings fails, the recovery
 *    entry is NOT removed and the function returns -1, allowing a retry.
 *
 * @param disk      Open filesystem image.
 * @param sb        Loaded superblock.
 * @param data_tree B-tree handle for file data spans (bspan_t mappings).
 * @param rec_tree  B-tree handle for recovery entries (rec_entry_t).
 * @param rec       The recovery entry describing the file to purge.
 * @return 0 on success; -1 on error.
 */
int purge_file(FILE *disk, superblock_t *sb, btree_handle_t *data_tree,
               btree_handle_t *rec_tree, const rec_entry_t *rec);

#endif /* REC_ENTRY_OPERATIONS_BSFS_H */
