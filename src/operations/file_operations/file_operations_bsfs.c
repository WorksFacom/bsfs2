/**
 * @file file_operations_bsfs.c
 * @brief Implementation of file-level operations in the BSFS file system.
 *
 * This file provides functions to manipulate files, including opening,
 * closing, reading, writing, and seeking, by utilizing inodes and
 * B-tree-based address mapping. Files are loaded into memory as `file_t`
 * structures which manage stateful access.
 */
#include "../../include/file_operations_bsfs.h"

/**
 * @brief Opens a file by loading its inode into memory.
 *
 * This function looks up the specified inode number in the inode B-tree,
 * and populates the `file_t` structure with its contents.
 *
 * @param disk Pointer to the open disk file.
 * @param inode_tree Pointer to the B-tree of inodes.
 * @param inode_number The inode number of the file to open.
 * @param out_file Pointer to the file_t structure to populate.
 * @return 0 on success, -1 on failure (e.g., inode not found).
 */
int open_file(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number,
              file_t *out_file) {
  if (!disk || !inode_tree || !out_file)
    return -1;

  inode_t inode;
  if (!read_inode(disk, inode_tree, inode_number, &inode))
    return -1;

  out_file->inode = inode;
  out_file->pos = 0;
  return 0;
}

/**
 * @brief Closes an open file and writes back its inode.
 *
 * This function persists any changes made to the file's inode
 * (e.g., size updates) by writing it back into the inode B-tree.
 *
 * @param disk Pointer to the open disk file.
 * @param inode_tree Pointer to the B-tree of inodes.
 * @param file Pointer to the file_t structure representing the open file.
 * @return 0 on success, -1 on failure.
 */
int close_file(FILE *disk, btree_handle_t *inode_tree, file_t *file) {
  if (!disk || !inode_tree || !file)
    return -1;
  return update_inode(disk, inode_tree, &file->inode);
}

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
int32_t seek_file(file_t *file, int32_t offset, int whence) {
  if (!file)
    return -1;

  int32_t new_pos = -1;

  switch (whence) {
  case SEEK_SET:
    new_pos = offset;
    break;
  case SEEK_CUR:
    new_pos = (int32_t)file->pos + offset;
    break;
  case SEEK_END:
    new_pos = (int32_t)file->inode.file_size + offset;
    break;
  default:
    return -1;
  }

  if (new_pos < 0 || (uint32_t)new_pos > file->inode.file_size)
    return -1;

  file->pos = (uint32_t)new_pos;
  return file->pos;
}

/**
 * @brief Reads data from an open file into a buffer.
 *
 * Reads up to `size` bytes from the current position in the file into `buffer`.
 * The function checks direct `bspan_t` spans in the inode first, then falls
 * back to the B-tree for additional block mappings. If a block is not mapped,
 * it is treated as zeroed.
 *
 * @param disk Pointer to the disk image file.
 * @param handle B-tree handle used for indirect block mapping.
 * @param file Open file structure (with inode and current position).
 * @param buffer Buffer to read data into.
 * @param size Number of bytes to read.
 * @return Number of bytes successfully read.
 */
int read_file(FILE *disk, btree_handle_t *handle, file_t *file, uint8_t *buffer,
              uint32_t size) {
  uint32_t pos = file->pos;
  uint32_t file_size = file->inode.file_size;

  if (pos >= file_size)
    return 0;
  if (pos + size > file_size)
    size = file_size - pos;

  uint32_t bytes_read = 0;

  while (bytes_read < size) {
    uint32_t logical_block = pos / BLOCK_SIZE;
    uint32_t block_offset = pos % BLOCK_SIZE;
    uint32_t to_read = BLOCK_SIZE - block_offset;
    if (to_read > size - bytes_read)
      to_read = size - bytes_read;

    uint32_t disk_block = INVALID_BLOCK;
    int found = 0;

    // Search in direct bspan
    for (int i = 0; i < 16; i++) {
      bspan_t span = file->inode.direct_blocks[i];
      if (span.length == 0)
        continue;

      if (logical_block >= span.file_blk &&
          logical_block < span.file_blk + span.length) {
        disk_block = span.disk_blk + (logical_block - span.file_blk);
        found = 1;
        break;
      }
    }

    // Search in the tree
    if (!found && handle && file->inode.btree_root != 0) {
      bspan_t result;
      if (btree_search(disk, handle, file->inode.btree_root, logical_block,
                       &result)) {
        disk_block = result.disk_blk + (logical_block - result.file_blk);
        found = 1;
      }
    }

    uint8_t temp[BLOCK_SIZE] = {0};
    if (found) {
      read_block(disk, disk_block, temp, BLOCK_SIZE);
      memcpy(buffer + bytes_read, temp + block_offset, to_read);
    } else {
      memset(buffer + bytes_read, 0, to_read); // Empty block
    }

    pos += to_read;
    bytes_read += to_read;
  }

  file->pos = pos;
  return bytes_read;
}

/**
 * @brief Truncates a file from a given byte offset, removing its excess data
 * blocks.
 *
 * This function removes all bspan mappings beyond the given logical file size,
 * effectively truncating the file and freeing corresponding disk blocks.
 *
 * It updates the file's inode size and adjusts the read/write position if
 * needed.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock structure.
 * @param bspan_tree B-tree handle that maps logical file blocks to disk blocks.
 * @param f Pointer to the open file structure.
 * @param new_size The new desired file size in bytes.
 * @return 0 on success, -1 on error.
 */
int truncate_file(FILE *disk, superblock_t *sb, btree_handle_t *bspan_tree,
                  file_t *f, uint32_t new_size) {
  if (new_size >= f->inode.file_size)
    return 0; // Nothing to truncate

  uint32_t block_size = BLOCK_SIZE;
  uint32_t file_blk =
      (new_size + block_size - 1) / block_size; // First block to remove

  // Iterate through logical blocks starting from file_blk
  while (1) {
    bspan_t bspan;
    if (!btree_search(disk, bspan_tree, bspan_tree->root_block, file_blk,
                      &bspan))
      break; // No more blocks to remove

    // Remove from B-tree
    if (!btree_remove(disk, sb, bspan_tree, file_blk)) {
      fprintf(stderr, "Failed to remove bspan at file_blk=%u\n", file_blk);
      return -1;
    }

    // Free physical block
    if (deallocate_bspan(disk, sb, &bspan) != 0) {
      fprintf(stderr,
              "Failed to deallocate disk block at file_blk=%u (disk_blk=%u)\n",
              bspan.file_blk, bspan.disk_blk);
      return -1;
    }

    file_blk++; // Proceed to next logical block
  }

  // Update inode metadata
  f->inode.file_size = new_size;
  if (f->pos > new_size)
    f->pos = new_size;

  return 0;
}

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
               file_t *f, const uint8_t *buffer, uint32_t size, int truncate) {
  if (size == 0)
    return 0;

  uint32_t pos = f->pos;
  uint32_t block_size = BLOCK_SIZE;
  uint32_t final_pos = pos + size;

  // Truncation if requested
  if (truncate && final_pos < f->inode.file_size) {
    if (truncate_file(disk, sb, bspan_tree, f, final_pos) != 0) {
      return -1;
    }
  }

  // Extra blocks
  uint32_t new_bytes =
      (final_pos > f->inode.file_size) ? (final_pos - f->inode.file_size) : 0;
  uint32_t new_blocks = (new_bytes + block_size - 1) / block_size;

  uint32_t hint_offset = 0; // Pode ser heurístico (ex: baseado no inode)
  bspan_queue_t *queue;
  allocate_bspan_queue(disk, sb, queue, new_blocks, hint_offset);
  if (!queue) {
    return -1;
  }

  // Escrita por blocos
  uint32_t bytes_written = 0;

  while (bytes_written < size) {
    uint32_t logical_block = pos / block_size;
    uint32_t block_offset = pos % block_size;
    uint32_t to_write = block_size - block_offset;
    if (to_write > size - bytes_written)
      to_write = size - bytes_written;

    // Allocate bspan
    bspan_t bspan;
    if (!btree_search(disk, bspan_tree, bspan_tree->root_block, logical_block,
                      &bspan)) {
      if (is_bspan_queue_empty(queue))
        break;

      bspan = dequeue_bspan(queue);
      bspan.file_blk = logical_block;
      if (!btree_insert(disk, sb, bspan_tree, &bspan)) {
        fprintf(stderr, "Failed to insert bspan at file_blk=%u\n",
                logical_block);
        break;
      }
    }

    // Partial read
    uint8_t temp[BLOCK_SIZE] = {0};
    if (block_offset != 0 || to_write < block_size) {
      read_block(disk, bspan.disk_blk, temp, BLOCK_SIZE);
    }

    memcpy(temp + block_offset, buffer + bytes_written, to_write);
    write_block(disk, bspan.disk_blk, temp, BLOCK_SIZE);

    pos += to_write;
    bytes_written += to_write;
  }

  // Update file size
  if (pos > f->inode.file_size)
    f->inode.file_size = pos;

  f->pos = pos;

  free_bspan_queue(queue);

  return bytes_written;
}
