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

#include "dentry.h"
#include "fs.h"
#include "inode.h"
#include "path_utils.h"
#include "rec_entry.h"

/**
 * @brief Logically remove a file and snapshot it into the Recovery tree.
 *
 * Unlinks the directory entry for @p path, removes the file's inode from the
 * inode tree (without freeing its data blocks), and inserts a snapshot of the
 * file metadata into the Recovery B-tree to enable later restoration or purge.
 *
 * Steps:
 *  1) Resolve the parent directory and basename from @p path.
 *  2) Lookup the file inode (must not be a directory).
 *  3) Build and insert a @ref rec_entry_t snapshot into the Recovery tree.
 *  4) Remove the directory entry from its parent.
 *  5) Remove the inode from the inode tree.
 *  6) Update parent directory timestamps.
 *
 * Rollback: If step 4 or 5 fails, the inserted recovery entry is removed to
 * keep metadata consistent.
 *
 * @param disk       Open filesystem image.
 * @param sb         Loaded superblock.
 * @param inode_tree Inode B-tree handle.
 * @param rec_tree   Recovery B-tree handle.
 * @param path       Absolute or relative path of the file to remove.
 * @param cwd        Current working directory (for resolving relative paths).
 * @param uid        Caller user id (currently unused; permission checks may use
 * it).
 * @return 0 on success; -1 on error.
 */
int remove_file(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree, btree_handle_t *rec_tree, const char *path, dir_t *cwd,
                uint16_t uid);

#endif /* REMOVE_FILE_BSFS_H */
