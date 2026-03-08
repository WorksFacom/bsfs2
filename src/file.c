/**
 * @file file_operations_bsfs.c
 * @brief Implementation of file-level operations in the BSFS file system.
 *
 * This file provides functions to manipulate files, including opening,
 * closing, reading, writing, and seeking, by utilizing inodes and
 * B-tree-based address mapping. Files are loaded into memory as `file_t`
 * structures which manage stateful access.
 */
#include "../include/file.h"

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
 * @brief Search the bspan tree for the span with the greatest file_blk <= key.
 *
 * Performs a B-tree walk to find the predecessor span for @p key, if any. This
 * enables treating `bspan_t` entries as intervals during reads/writes.
 *
 * @param disk Pointer to the open disk file.
 * @param handle B-tree handle of bspan entries (root_block must be set).
 * @param key Logical file block to search predecessor for.
 * @param out Output: predecessor bspan entry if found.
 * @return 1 if found, 0 otherwise.
 */
static int bspan_search_le(FILE *disk, btree_handle_t *handle, uint32_t key,
                           bspan_t *out) {
  if (!handle || handle->root_block == 0)
    return 0;
  uint32_t node_block = handle->root_block;
  bspan_t best = {0};
  int have = 0;

  while (node_block) {
    btree_node_t *node = btree_read_node(disk, handle, node_block);
    if (!node)
      break;
    uint8_t *entries = node->entries;
    uint32_t i = 0;
    // find first entry with key >= target
    while (i < node->num_entries) {
      uint32_t ekey = handle->get_key(entries + i * handle->entry_size);
      if (ekey > key)
        break;
      // this entry is <= key; candidate predecessor
      memcpy(&best, entries + i * handle->entry_size, sizeof(best));
      have = 1;
      i++;
    }
    if (node->is_leaf) {
      btree_free_node(node);
      break;
    }
    // descend to child i
    uint32_t child = node->children[i];
    btree_free_node(node);
    node_block = child;
  }
  if (have) {
    *out = best;
    return 1;
  }
  return 0;
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

    // Search in the tree: find span with file_blk <= logical_block
    if (!found && handle && file->inode.btree_root != 0) {
      bspan_t result;
      handle->root_block = file->inode.btree_root;
      if (bspan_search_le(disk, handle, logical_block, &result)) {
        if (logical_block >= result.file_blk &&
            logical_block < result.file_blk + result.length) {
          disk_block = result.disk_blk + (logical_block - result.file_blk);
          found = 1;
        }
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
  uint32_t first_remove_blk = (new_size + block_size - 1) / block_size;

  // 1) Shrink overlapping head span if needed
  bspan_t span;
  if (bspan_search_le(disk, bspan_tree, first_remove_blk, &span)) {
    if (first_remove_blk < span.file_blk + span.length) {
      // free tail portion beyond new_size
      bspan_t tail = span;
      uint32_t keep_len = (first_remove_blk > span.file_blk)
                              ? (first_remove_blk - span.file_blk)
                              : 0;
      tail.file_blk = first_remove_blk;
      tail.disk_blk = span.disk_blk + keep_len;
      tail.length = span.length - keep_len;
      if (tail.length > 0) {
        if (deallocate_bspan(disk, sb, &tail) != 0)
          return -1;
      }
      // shrink original span to keep portion
      span.length = keep_len;
      if (keep_len > 0) {
        (void)btree_update(disk, bspan_tree, bspan_tree->root_block, &span);
      } else {
        // remove empty span entry
        (void)btree_remove(disk, sb, bspan_tree, span.file_blk);
      }
    }
  }

  // 2) Collect and remove all spans starting at or after first_remove_blk
  // Traverse tree and collect keys >= threshold
  typedef struct {
    uint32_t k;
  } krec;
  krec keys[1024];
  size_t kcount = 0;
  // simple DFS stack of node blocks
  uint32_t stack[128];
  int top = 0;
  if (bspan_tree->root_block)
    stack[top++] = bspan_tree->root_block;
  while (top > 0) {
    uint32_t nb = stack[--top];
    btree_node_t *node = btree_read_node(disk, bspan_tree, nb);
    if (!node)
      continue;
    uint8_t *entries = node->entries;
    // push children first for post-order like
    if (!node->is_leaf) {
      for (uint32_t i = 0; i <= node->num_entries; i++) {
        if (top < (int)(sizeof(stack) / sizeof(stack[0])))
          stack[top++] = node->children[i];
      }
    }
    for (uint32_t i = 0; i < node->num_entries; i++) {
      uint32_t ekey = bspan_tree->get_key(entries + i * bspan_tree->entry_size);
      if (ekey >= first_remove_blk) {
        if (kcount < sizeof(keys) / sizeof(keys[0])) {
          keys[kcount++].k = ekey;
        }
      }
    }
    btree_free_node(node);
  }
  // Remove collected keys
  for (size_t i = 0; i < kcount; i++) {
    bspan_t bs;
    if (btree_search(disk, bspan_tree, bspan_tree->root_block, keys[i].k,
                     &bs)) {
      (void)deallocate_bspan(disk, sb, &bs);
      (void)btree_remove(disk, sb, bspan_tree, keys[i].k);
    }
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

  uint32_t hint_offset = 0;
  bspan_queue_t queue;
  init_bspan_queue(&queue);
  if (new_blocks > 0) {
    if (allocate_bspan_queue(disk, sb, &queue, new_blocks, hint_offset) != 0) {
      fprintf(stderr,
              "write_file: allocation failed (ino=%u, new_blocks=%u, size=%u)\n",
              f->inode.inode_number, new_blocks, size);
      return -1;
    }
  }

  // Write blocks
  uint32_t bytes_written = 0;

  while (bytes_written < size) {
    uint32_t logical_block = pos / block_size;
    uint32_t block_offset = pos % block_size;
    uint32_t to_write = block_size - block_offset;
    if (to_write > size - bytes_written)
      to_write = size - bytes_written;

    // Allocate bspan
    bspan_t bspan;
    if (!bspan_search_le(disk, bspan_tree, logical_block, &bspan) ||
        !(logical_block >= bspan.file_blk &&
          logical_block < bspan.file_blk + bspan.length)) {
      if (is_bspan_queue_empty(&queue)) {
        fprintf(stderr,
                "write_file: span queue empty (ino=%u, logical=%u, size=%u)\n",
                f->inode.inode_number, logical_block, size);
        break;
      }

      bspan = dequeue_bspan(&queue);
      bspan.file_blk = logical_block;
      if (btree_insert(disk, sb, bspan_tree, &bspan) != 0) {
        fprintf(stderr,
                "write_file: btree_insert failed (ino=%u, logical=%u, disk=%u, "
                "len=%u)\n",
                f->inode.inode_number, logical_block, bspan.disk_blk,
                bspan.length);
        break;
      }
      if (f->inode.btree_root == 0 && bspan_tree->root_block != 0)
        f->inode.btree_root = bspan_tree->root_block;
    }

    uint32_t phys_block = bspan.disk_blk + (logical_block - bspan.file_blk);

    // Partial read
    uint8_t temp[BLOCK_SIZE] = {0};
    if (block_offset != 0 || to_write < block_size) {
      read_block(disk, phys_block, temp, BLOCK_SIZE);
    }

    memcpy(temp + block_offset, buffer + bytes_written, to_write);
    write_block(disk, phys_block, temp, BLOCK_SIZE);

    pos += to_write;
    bytes_written += to_write;
  }

  // Update file size
  if (pos > f->inode.file_size)
    f->inode.file_size = pos;

  if (bspan_tree->root_block != 0)
    f->inode.btree_root = bspan_tree->root_block;

  if (f->inode.file_size == 0) {
    f->inode.block_count = 0;
  } else {
    uint32_t used_blocks =
        (f->inode.file_size + block_size - 1) / block_size;
    f->inode.block_count = used_blocks;
  }

  f->pos = pos;

  free_bspan_queue(&queue);

  return bytes_written;
}
