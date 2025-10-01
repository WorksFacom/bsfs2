/**
 * @file create_file_bsfs.h
 * @brief Interface for file creation operations in the BSFS filesystem.
 *
 * This header exposes functions to create new files in the BSFS
 * filesystem. File creation involves resolving the parent directory,
 * validating uniqueness of the filename, allocating and initializing
 * a new inode, inserting it into the inode B-tree, and linking the
 * inode into the directory hierarchy with a directory entry.
 *
 * Provided functions:
 *  - create_file(): create a new empty regular file.
 *
 * This header is part of the public API for higher-level file operations.
 */
#ifndef FILE_CREATE_BSFS_H
#define FILE_CREATE_BSFS_H

#include "directory_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include "inode_operations_bsfs.h"
#include "path_utils_bsfs.h"

/**
 * @brief Creates an empty regular file at the given path.
 *
 * This function initializes a fresh @ref inode_t (size = 0, no data blocks),
 * inserts it into the inode B-tree, and then adds a directory entry in the
 * parent directory resolved from @p path. It prevents duplicate names in the
 * parent directory and updates the parent's metadata (timestamps).
 *
 * Workflow:
 *  1) Resolve the parent directory and basename from @p path (relative to @p
 * cwd if needed) using resolve_parent_dir(). 2) Allocate a new inode number and
 * initialize a zero-sized regular file inode. 3) Insert the inode into the
 * inode B-tree via insert_inode(). 4) Build a dir_entry_t and insert it into
 * the parent directory using add_dir_entry(). 5) Update the parent directory's
 * timestamps and persist them via update_inode().
 *
 * Notes:
 *  - This routine does NOT allocate any data blocks; writes will allocate on
 * demand.
 *  - On failure to add the directory entry, the newly inserted inode is removed
 *    from the inode B-tree (basic rollback).
 *
 * @param disk         Open filesystem image.
 * @param sb           Loaded superblock.
 * @param inode_tree   Handle for the inode B-tree.
 * @param path         Absolute or relative path of the new file.
 * @param cwd          Current working directory for resolving relative paths.
 * @param uid          Owner user ID for the new inode.
 * @param gid          Owner group ID for the new inode.
 * @param permissions  Mode bits (e.g., 0644).
 *
 * @return 0 on success; -1 on error (invalid args, path resolution failure,
 *         inode insertion failure, duplicate name, or dir entry insertion
 * failure).
 */
int create_file(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                const char *path, dir_t *cwd, uint16_t uid, uint16_t gid,
                uint16_t permissions);

#endif // FILE_CREATE_BSFS_H
