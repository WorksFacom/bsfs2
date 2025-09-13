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
 * @brief Open a file given a path and load its inode into a file_t.
 * @return 0 on success; -1 on failure.
 */
int open_bsfs(FILE *disk, superblock_t *sb, const char *path, file_t *file);

/**
 * @brief Close an open file (no-op; metadata is persisted by write_bsfs).
 * @return 0 on success; -1 on failure.
 */
int close_bsfs(file_t *file);

/**
 * @brief Set the current file position (absolute).
 * @return 0 on success; -1 on invalid position.
 */
int seek_bsfs(file_t *file, uint32_t pos);

/**
 * @brief Read up to length bytes from current file position into data.
 * @return Number of bytes read (0 at EOF or on error).
 */
size_t read_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                 uint32_t length);

/**
 * @brief Write length bytes from data into file at current position.
 * @return Number of bytes written (0 on error).
 */
size_t write_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                  uint32_t length);

/**
 * @brief Overwrite a file with the provided data (truncate to zero, then write).
 * @return Number of bytes written (0 on error).
 */
size_t overwrite_bsfs(FILE *disk, superblock_t *sb, const char *path,
                      const void *data, uint32_t length);

/**
 * @brief Create an empty regular file at the given path.
 * @return 0 on success; -1 on failure.
 */
int create_file_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Logically remove a file: unlink + move snapshot into Recovery tree.
 * @return 0 on success; -1 on failure.
 */
int remove_file_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Move or rename a file (or directory) from one path to another.
 * @return 0 on success; -1 on failure.
 */
int move_bsfs(FILE *disk, superblock_t *sb, const char *old_path,
              const char *new_path);

/**
 * @brief Retrieve the inode associated with a path.
 * @return 0 on success; -1 on failure (not found).
 */
int stat_bsfs(FILE *disk, superblock_t *sb, const char *path, inode_t *inode);

/**
 * @brief Open a directory for iteration.
 * @return 0 on success; -1 on failure.
 */
int opendir_bsfs(FILE *disk, superblock_t *sb, const char *dirname, dir_t *dir);

/**
 * @brief Close a directory (no-op).
 * @return 0 on success.
 */
int closedir_bsfs(dir_t *dir);

/**
 * @brief Read next directory entry.
 * @return 1 on success, 0 on end of directory, -1 on error.
 */
int readdir_bsfs(FILE *disk, superblock_t *sb, dir_t *dir, dir_entry_t *entry);

/**
 * @brief Create a new directory at path.
 * @return 0 on success; -1 on failure.
 */
int mkdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Remove an empty directory at path.
 * @return 0 on success; -1 on failure.
 */
int rmdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Change current working directory to path.
 * @return 0 on success; -1 on failure.
 */
int chdir_bsfs(FILE *disk, superblock_t *sb, const char *path);

/**
 * @brief Get textual path of current working directory.
 * @return 0 on success; -1 on failure.
 */
int getcwd_bsfs(FILE *disk, superblock_t *sb, char *path_buffer,
                size_t buffer_size);

/**
 * @brief Restore a file from the Recovery tree by recovery ID.
 *
 * Loads the snapshot identified by @p recovery_id from the Recovery B-tree,
 * chooses a destination directory (explicit @p dest_dir if provided, otherwise
 * attempts the original parent recorded in the snapshot, falling back to the
 * current working directory), recreates the inode and directory entry, and
 * removes the snapshot on success.
 *
 * @param disk        Open filesystem image.
 * @param sb          Loaded superblock.
 * @param recovery_id Unique ID of the recovery snapshot.
 * @param dest_dir    Optional destination directory path (may be NULL).
 * @return 0 on success; -1 on failure.
 */
int restore_bsfs(FILE *disk, superblock_t *sb, uint32_t recovery_id,
                 const char *dest_dir);

/* Session identity and umask (used by permission checks) */
int set_identity_bsfs(uint16_t uid, uint16_t gid);
int get_identity_bsfs(uint16_t *uid, uint16_t *gid);
int set_umask_bsfs(uint16_t umask);
int get_umask_bsfs(uint16_t *umask);

/* Metadata operations with permission checks */
int chmod_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t mode);
int chown_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t uid);
int chgrp_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t gid);

#endif // API_BSFS_H
