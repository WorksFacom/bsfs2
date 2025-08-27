/**
 * @file remove_file_bsfs.h
 * @brief Interface for logical file removal in BSFS.
 *
 * This header defines the API to "remove" a file in BSFS. Removal in this
 * context is logical: the file is unlinked from its parent directory and its
 * inode is removed from the inode B-tree, but its data blocks are NOT freed.
 * Instead, a snapshot is inserted into the Recovery B-tree, enabling later
 * restoration of the file or eventual purging by a garbage collector.
 */

#ifndef REMOVE_FILE_BSFS_H
#define REMOVE_FILE_BSFS_H

#include "dir_entry_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include "inode_operations_bsfs.h"
#include "path_utils_bsfs.h"
#include "rec_entry_operations_bsfs.h"

/**
 * @brief Logically remove a file, moving it into the recovery tree.
 *
 * Steps:
 *  - Resolve parent directory and basename from @p path.
 *  - Look up the file's inode.
 *  - Build a @ref rec_entry_t snapshot of the file at deletion time.
 *  - Insert the recovery entry into the Recovery B-tree.
 *  - Remove the directory entry from the parent directory.
 *  - Remove the inode from the inode B-tree (data blocks remain allocated).
 *
 * @param disk       Open filesystem image.
 * @param sb         Loaded superblock.
 * @param inode_tree Handle for the inode B-tree.
 * @param rec_tree   Handle for the Recovery B-tree.
 * @param path       Absolute or relative path of the file to remove.
 * @param cwd        Current working directory for resolving relative paths.
 * @param uid        User ID of the calling process (for permission checks).
 * @return 0 on success; -1 on failure.
 */
int remove_file(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                btree_handle_t *rec_tree, const char *path, dir_t *cwd,
                uint16_t uid);

#endif /* REMOVE_FILE_BSFS_H */
