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
 * Steps performed:
 *  - Resolves the parent directory and final name component from @p path.
 *  - Verifies that no entry with the same name exists in the parent.
 *  - Allocates and initializes a fresh inode (size 0, no blocks allocated).
 *  - Inserts the inode into the inode B-tree.
 *  - Creates a directory entry in the parent directory pointing to the inode.
 *  - Updates the parent's metadata (timestamps).
 *
 * @param disk         Open filesystem image.
 * @param sb           Loaded superblock (may be updated if inode root changes).
 * @param inode_tree   Handle for the inode B-tree.
 * @param path         Absolute or relative file path of the file to create.
 * @param cwd          Current working directory context for relative
 * resolution.
 * @param uid          Owner user id.
 * @param gid          Owner group id.
 * @param permissions  Mode bits (e.g., 0644).
 * @return 0 on success; -1 on failure (invalid path, duplicate name,
 *         inode insertion failure, or directory entry insertion failure).
 */
int create_file(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                const char *path, dir_t *cwd, uint16_t uid, uint16_t gid,
                uint16_t permissions);

#endif // FILE_CREATE_BSFS_H
