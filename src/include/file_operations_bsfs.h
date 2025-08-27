/**
 * @file file_operations_bsfs.h
 * @brief Header file for file-level operations in the BSFS file system.
 *
 * This module provides functions for opening, closing, seeking, reading,
 * and writing files using the BSFS file system. File access is performed
 * through a memory-resident `file_t` structure, which maintains a cursor
 * and an associated inode.
 */

#ifndef FILE_OPERATIONS_BSFS_H
#define FILE_OPERATIONS_BSFS_H

#include "allocate_bsfs.h"
#include "block_operations_bsfs.h"
#include "bspan_queue_bsfs.h"
#include "btree_bsfs.h"
#include "dir_entry_operations_bsfs.h"
#include "directory_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include "inode_operations_bsfs.h"
#include "superblock_operations_bsfs.h"
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Opens a file by reading its inode into memory.
 *
 * @param disk Pointer to the open disk file.
 * @param inode_tree Pointer to the inode B-tree handle.
 * @param inode_number The number of the inode to open.
 * @param out_file Pointer to the file_t structure to populate.
 * @return 0 on success, -1 on failure.
 */
int open_file(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number,
              file_t *out_file);

/**
 * @brief Closes a file and writes back any inode metadata changes.
 *
 * @param disk Pointer to the open disk file.
 * @param inode_tree Pointer to the inode B-tree handle.
 * @param file Pointer to the open file to close.
 * @return 0 on success, -1 on failure.
 */
int close_file(FILE *disk, btree_handle_t *inode_tree, file_t *file);

/**
 * @brief Moves the file position indicator to a new location.
 *
 * This function adjusts the `pos` field of the file_t structure,
 * which determines the current read/write position in the file.
 *
 * The behavior of the seek operation depends on the `whence` parameter:
 * - SEEK_SET: position is set to `offset`
 * - SEEK_CUR: position is moved by `offset` from the current position
 * - SEEK_END: position is set to file size + `offset`
 *
 * @param file Pointer to the open file structure.
 * @param offset Number of bytes to move the position indicator.
 * @param whence Starting point for the offset: SEEK_SET, SEEK_CUR, SEEK_END.
 * @return The new file position on success, or -1 on failure.
 */
int32_t seek_file(file_t *file, int32_t offset, int whence);

/**
 * @brief Reads data from the file into the provided buffer.
 *
 * @param disk Pointer to the open disk file.
 * @param bspan_tree Pointer to the B-tree for block span lookup.
 * @param file Pointer to the open file.
 * @param buffer Output buffer where data will be stored.
 * @param size Number of bytes to read.
 * @return Number of bytes successfully read, or -1 on error.
 */
int read_file(FILE *disk, btree_handle_t *bspan_tree, file_t *file,
              uint8_t *buffer, uint32_t size);

/**
 * @brief Writes data to a file, with optional truncation.
 *
 * This function writes `size` bytes from `buffer` into the file starting at
 * `f->pos`. It optionally truncates the file from that position before writing,
 * based on the `truncate` flag:
 *
 * - truncate == 0: overwrite data starting at `f->pos`, preserve what's after.
 * - truncate == 1: remove all data after `f->pos` before writing.
 * - If `f->pos == inode.size`, this acts as append (with or without
 * truncation).
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock (for allocation and deallocation).
 * @param bspan_tree B-tree handle mapping logical to physical blocks.
 * @param f Pointer to the open file structure.
 * @param buffer Pointer to the data to write.
 * @param size Number of bytes to write.
 * @param truncate 1 to truncate after position before writing, 0 to preserve.
 * @return Number of bytes written, or -1 on error.
 */
int write_file(FILE *disk, superblock_t *sb, btree_handle_t *bspan_tree,
               file_t *f, const uint8_t *buffer, uint32_t size, int truncate);

#endif // FILE_OPERATIONS_BSFS_H
