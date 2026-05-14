/**
 * @file api_bsfs.h
 * @brief High-level API used by the BSFS browser shell.
 *
 * This header declares helper functions that wrap the internal BSFS
 * operations (inode/directory/bspan B-trees) to provide a simple interface to
 * the interactive browser. Functions support absolute and relative paths; when
 * a relative path is provided, it is resolved against the in-memory current
 * working directory maintained by the API.
 */

#ifndef API_BSFS_H
#define API_BSFS_H

#include "../bsfs.h"   // bsfs_super_block, bsfs_inode, file_t
#include "rec_entry.h" // recovery tree helpers/APIs

/**
 * @brief Logically remove a file and move its snapshot into the Recovery tree.
 *
 * Performs parent directory permissions and sticky-bit checks before unlinking
 * the directory entry, removing the inode from the inode tree, and inserting a
 * snapshot into the Recovery B-tree to allow later restore.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target file path.
 * @return 0 on success; -1 on error.
 */
// int remove_file_bsfs(FILE *disk, bsfs_super_block *sb, const char *path);

/**
 * @brief Remove a file and immediately purge its Recovery snapshot.
 *
 * Performs the logical removal (same as @ref remove_file_bsfs) and then
 * reclaims the file's data blocks by purging any Recovery entries that match
 * the deleted file's basename and parent directory.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target file path.
 * @return 0 on success; -1 on error.
 */
// int delete_file_permanently_bsfs(FILE *disk, bsfs_super_block *sb, const char *path);

/**
 * @brief Move or rename an entry from @p old_path to @p new_path.
 *
 * Resolves source/destination parents, checks modify permissions on both and
 * sticky-bit semantics on the source, then relinks the entry with the new
 * name/location.
 *
 * @param disk      Open filesystem image.
 * @param sb        Loaded superblock.
 * @param old_path  Source path.
 * @param new_path  Destination path (may include a new basename).
 * @return 0 on success; -1 on error.
 */
// int move_bsfs(FILE *disk, bsfs_super_block *sb, const char *old_path, const char *new_path);

/**
 * @brief Restore a deleted file by recovery ID.
 *
 * Loads the snapshot from the Recovery B-tree, chooses the destination
 * directory (explicit, original parent, or CWD), recreates the inode and
 * directory entry, and removes the snapshot. Restricted to root by default.
 *
 * @param disk         Open filesystem image.
 * @param sb           Loaded superblock.
 * @param recovery_id  Recovery entry identifier.
 * @param dest_dir     Optional destination directory path.
 * @return 0 on success; -1 on error or insufficient permission.
 */
// int restore_bsfs(FILE *disk, bsfs_super_block *sb, uint32_t recovery_id, const char *dest_dir);

#endif // API_BSFS_H
