/**
 * @file restore_file_bsfs.h
 * @brief Interface for restoring files from the Recovery tree in BSFS.
 *
 * This header declares high-level routines to restore a previously deleted
 * file by using its snapshot stored in the Recovery B-tree. Restoration
 * recreates the inode (reusing the recorded block spans) and links a new
 * directory entry at the chosen destination directory. The corresponding
 * recovery entry is then removed from the Recovery tree.
 */
#ifndef RESTORE_FILE_BSFS_H
#define RESTORE_FILE_BSFS_H

#include "dir_entry_operations_bsfs.h"
#include "directory_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include "inode_operations_bsfs.h"
#include "path_utils_bsfs.h"
#include "rec_entry_operations_bsfs.h"
#include "superblock_operations_bsfs.h"

/**
 * @brief Restore a file from the Recovery tree by its recovery ID.
 *
 * Semantics:
 *  - Loads the snapshot from the Recovery B-tree using @p recovery_id.
 *  - Determines the destination directory: if @p dest_dir_path is non-NULL,
 *    resolves it to a directory inode (relative to @p cwd if needed). If NULL
 *    or resolution fails, attempts to use the original parent recorded in the
 *    snapshot; if that is not available, falls back to @p cwd.
 *  - Resolves name conflicts by appending a suffix containing the recovery ID,
 *    ensuring a unique filename in the destination directory.
 *  - Recreates the inode from the snapshot (reusing its block mapping) and
 *    inserts it into the inode B-tree, updating @p sb->inode_root if the
 *    insertion creates the tree root.
 *  - Creates a directory entry in the destination directory and updates its
 *    metadata timestamps.
 *  - Removes the entry from the Recovery tree.
 *
 * @param disk           Open filesystem image.
 * @param sb             Loaded superblock (may be updated if inode root is
 *                       first created during restore).
 * @param inode_tree     Inode B-tree handle (handle->root_block should be set
 *                       from @p sb->inode_root if it already exists).
 * @param rec_tree       Recovery B-tree handle (handle->root_block should be
 *                       set from @p sb->recovery_root).
 * @param recovery_id    Unique ID of the recovery snapshot to restore.
 * @param dest_dir_path  Optional destination directory path (absolute or
 *                       relative to @p cwd). If NULL, attempt original parent
 *                       then @p cwd.
 * @param cwd            Current working directory context for resolving
 *                       relative paths and as a fallback destination.
 * @return 0 on success; -1 on failure (snapshot not found, invalid destination,
 *         insertion or linking errors).
 */
int restore_file_by_id(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                       btree_handle_t *rec_tree, uint32_t recovery_id,
                       const char *dest_dir_path, dir_t *cwd);

#endif /* RESTORE_FILE_BSFS_H */

