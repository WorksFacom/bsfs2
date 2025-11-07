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

// Centralize lower-level dependencies needed by browser commands
#include "btree_bsfs.h"                 // btree_handle_t, hash helpers
#include "dir_entry_operations_bsfs.h"  // dir_t, dir_entry_t
#include "filesystem_bsfs.h"            // superblock_t, inode_t, file_t
#include "rec_entry_operations_bsfs.h"  // recovery tree helpers/APIs
#include "superblock_operations_bsfs.h" // read/write superblock
#include "permissions_bsfs.h"           // DAC helpers

/**
 * @brief Open a file by path and load its inode into a file_t.
 *
 * Resolves @p path against the API's CWD, verifies that the target is a file
 * or directory (permission checks for reading/writing are enforced later in
 * read/write), and loads its inode into @p file with position set to 0.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (inode tree root is used for lookups).
 * @param path Absolute or relative path to the file.
 * @param file Output: populated file descriptor snapshot.
 * @return 0 on success; -1 on failure.
 */
int open_bsfs(FILE *disk, superblock_t *sb, const char *path, file_t *file);

/**
 * @brief Close an open file descriptor (no-op in this API).
 *
 * Metadata persistence is handled by write operations; closing does not flush
 * any state and always succeeds.
 *
 * @param file Open file descriptor (unused).
 * @return 0.
 */
int close_bsfs(file_t *file);

/**
 * @brief Set the current file position (absolute) within the open file.
 *
 * Fails if @p pos exceeds the current logical file size.
 *
 * @param file Open file descriptor.
 * @param pos  New absolute byte position from the start of file.
 * @return 0 on success; -1 on invalid arguments or position.
 */
int seek_bsfs(file_t *file, uint32_t pos);

/**
 * @brief Read up to @p length bytes from the current file position.
 *
 * Performs a permission check (read) for the current session identity and
 * reads data spanning direct and indirect block spans. Advances the file
 * position by the number of bytes read.
 *
 * @param disk   Open filesystem image.
 * @param sb     Loaded superblock.
 * @param file   Open file descriptor.
 * @param data   Output buffer.
 * @param length Maximum number of bytes to read.
 * @return Number of bytes read (0 on EOF or error).
 */
size_t read_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                 uint32_t length);

/**
 * @brief Write @p length bytes from @p data at the current file position.
 *
 * Allocates block spans as needed, writes the data, advances the file
 * position, and persists the inode metadata update in the inode tree.
 * Requires write permission for the current session identity.
 *
 * @param disk   Open filesystem image.
 * @param sb     Loaded superblock (used for allocation and inode updates).
 * @param file   Open file descriptor.
 * @param data   Input buffer containing bytes to write.
 * @param length Number of bytes to write.
 * @return Number of bytes written (0 on error).
 */
size_t write_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                  uint32_t length);

/**
 * @brief Truncate an open file to @p new_size bytes.
 *
 * Frees any blocks beyond @p new_size and updates the inode metadata.
 *
 * @param disk     Open filesystem image.
 * @param sb       Loaded superblock.
 * @param file     Open file descriptor (updated in place).
 * @param new_size Target logical size in bytes.
 * @return 0 on success; -1 on error.
 */
int truncate_file_bsfs(FILE *disk, superblock_t *sb, file_t *file,
                       uint32_t new_size);

/**
 * @brief Overwrite an entire file with the provided buffer.
 *
 * Opens @p path, seeks to 0, truncates to zero, and writes @p length bytes
 * from @p data. Updates the inode metadata accordingly.
 *
 * @param disk   Open filesystem image.
 * @param sb     Loaded superblock.
 * @param path   Target file path.
 * @param data   Buffer to write from.
 * @param length Number of bytes to write.
 * @return Number of bytes written (0 on failure).
 */
size_t overwrite_bsfs(FILE *disk, superblock_t *sb, const char *path,
                      const void *data, uint32_t length);

/**
 * @brief Create an empty regular file at @p path.
 *
 * Resolves the parent directory, checks modify permissions there, and creates
 * a zero-length inode with owner/mode derived from the current session identity
 * and umask.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Absolute or relative file path.
 * @return 0 on success; -1 on error (permissions, duplicates, or I/O).
 */
int create_file_bsfs(FILE *disk, superblock_t *sb, const char *path);

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
int remove_file_bsfs(FILE *disk, superblock_t *sb, const char *path);

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
int delete_file_permanently_bsfs(FILE *disk, superblock_t *sb,
                                 const char *path);

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
int move_bsfs(FILE *disk, superblock_t *sb, const char *old_path,
              const char *new_path);

/**
 * @brief Resolve @p path and return its inode metadata.
 *
 * @param disk  Open filesystem image.
 * @param sb    Loaded superblock.
 * @param path  Absolute or relative path.
 * @param inode Output: filled with the target's inode on success.
 * @return 0 on success; -1 if not found or invalid args.
 */
int stat_bsfs(FILE *disk, superblock_t *sb, const char *path, inode_t *inode);

/**
 * @brief Open a directory for iteration.
 *
 * If @p dirname is NULL or ".", opens the current CWD after checking traverse
 * and list permissions. Otherwise resolves @p dirname and verifies it is a
 * directory and accessible.
 *
 * @param disk    Open filesystem image.
 * @param sb      Loaded superblock.
 * @param dirname Directory path (NULL or "." means CWD).
 * @param dir     Output: directory descriptor snapshot.
 * @return 0 on success; -1 on error.
 */
int opendir_bsfs(FILE *disk, superblock_t *sb, const char *dirname, dir_t *dir);

/**
 * @brief Close a directory descriptor (no-op for this API).
 * @param dir Directory descriptor (unused).
 * @return 0.
 */
int closedir_bsfs(dir_t *dir);

/**
 * @brief Read the next directory entry in key order.
 *
 * Uses the underlying B-tree iteration. Returns 1 when an entry is produced,
 * 0 on end of directory, and -1 on invalid arguments.
 *
 * @param disk   Open filesystem image.
 * @param sb     Loaded superblock.
 * @param dir    Open directory descriptor.
 * @param entry  Output: next directory entry.
 * @return 1 on success, 0 on end, -1 on error.
 */
int readdir_bsfs(FILE *disk, superblock_t *sb, dir_t *dir, dir_entry_t *entry);

/**
 * @brief Create a new directory at @p path.
 *
 * Checks modify permissions on the parent and creates an empty directory with
 * entries for "." and "..". Owner and mode are set from session identity and
 * umask.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Directory path (absolute or relative).
 * @return 0 on success; -1 on error.
 */
int mkdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Remove an empty directory at @p path.
 *
 * Verifies the target is a directory, checks parent modify permission and
 * sticky-bit semantics, then removes the directory if it is empty.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Directory path to remove.
 * @return 0 on success; -1 on error.
 */
int rmdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Change the current working directory to @p path.
 *
 * Resolves the path, verifies it is a directory, and checks traverse
 * permission for the current session identity.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Directory path.
 * @return 0 on success; -1 on error.
 */
int chdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Get the textual representation of the current working directory.
 *
 * @param disk         Unused.
 * @param sb           Unused.
 * @param path_buffer  Output buffer to receive the path.
 * @param buffer_size  Capacity of @p path_buffer (bytes).
 * @return 0 on success; -1 on invalid arguments.
 */
int getcwd_bsfs(FILE *disk, superblock_t *sb, char *path_buffer,
                size_t buffer_size);

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
int restore_bsfs(FILE *disk, superblock_t *sb, uint32_t recovery_id,
                 const char *dest_dir);

/**
 * @brief Set the current session identity (uid,gid) for permission checks.
 * @param uid New user id.
 * @param gid New group id.
 * @return 0.
 */
int set_identity_bsfs(uint16_t uid, uint16_t gid);

/**
 * @brief Get the current session identity (uid,gid).
 * @param uid Output: current user id (nullable).
 * @param gid Output: current group id (nullable).
 * @return 0.
 */
int get_identity_bsfs(uint16_t *uid, uint16_t *gid);

/**
 * @brief Set the current session umask (000..777).
 * @param umask New umask in octal (lowest 9 bits are honored).
 * @return 0.
 */
int set_umask_bsfs(uint16_t umask);

/**
 * @brief Get the current session umask.
 * @param umask Output: current umask (nullable).
 * @return 0.
 */
int get_umask_bsfs(uint16_t *umask);

/**
 * @brief Change permission bits of a path (octal 0000..7777).
 *
 * Allowed to root or file owner.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target path.
 * @param mode New mode bits (lowest 12 bits are honored).
 * @return 0 on success; -1 on error or permission denied.
 */
int chmod_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t mode);

/**
 * @brief Change the owner (uid) of a path (root only).
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target path.
 * @param uid  New owner user id.
 * @return 0 on success; -1 on error or permission denied.
 */
int chown_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t uid);

/**
 * @brief Change the group (gid) of a path (root only).
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target path.
 * @param gid  New group id.
 * @return 0 on success; -1 on error or permission denied.
 */
int chgrp_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t gid);

#endif // API_BSFS_H
