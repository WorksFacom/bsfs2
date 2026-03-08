/**
 * @file inode_operations_bsfs.c
 * @brief Implementation of inode manipulation operations for BSFS.
 *
 * This source file provides functions to manage inode entries within the
 * inode B-tree in the BSFS file system.
 *
 * It implements:
 * - Insertion of new inodes into the tree
 * - Lookup and reading of inodes by their inode number
 * - Inode updates that overwrite existing metadata
 * - Deletion of inodes from the tree
 *
 * These functions encapsulate the B-tree logic and provide a clean interface
 * for interacting with inodes without requiring direct manipulation of tree
 * internals.
 */
#include <stdio.h>

#include "../include/inode.h"
#include "../include/super.h"

static uint32_t s_next_inode_number = 1;
static uint32_t s_cached_inode_root = INVALID_BLOCK;

static int inode_tree_find_max(FILE *disk, btree_handle_t *handle,
                               uint32_t node_block, inode_t *out_inode) {
  if (!handle || node_block == 0 || !out_inode)
    return 0;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  int ok = 0;
  if (node->is_leaf) {
    if (node->num_entries > 0) {
      memcpy(out_inode,
             (uint8_t *)node->entries +
                 (node->num_entries - 1) * handle->entry_size,
             handle->entry_size);
      ok = 1;
    }
  } else {
    uint32_t child_index = node->num_entries;
    if (child_index >= (uint32_t)(2 * handle->min_degree)) {
      child_index = (node->num_entries > 0) ? node->num_entries - 1 : 0;
    }
    uint32_t child_block = node->children[child_index];
    if (child_block != 0 && child_block != INVALID_BLOCK)
      ok = inode_tree_find_max(disk, handle, child_block, out_inode);
    if (!ok && node->num_entries > 0) {
      memcpy(out_inode,
             (uint8_t *)node->entries +
                 (node->num_entries - 1) * handle->entry_size,
             handle->entry_size);
      ok = 1;
    }
  }

  btree_free_node(node);
  return ok;
}

static void inode_sync_next(FILE *disk, superblock_t *sb,
                            btree_handle_t *inode_tree) {
  uint32_t root_block = inode_tree ? inode_tree->root_block : 0;
  if (sb)
    root_block = sb->inode_root;

  if (root_block == s_cached_inode_root)
    return;

  if (root_block == 0 || !inode_tree) {
    s_next_inode_number = 1;
    s_cached_inode_root = root_block;
    return;
  }

  inode_t max_inode;
  if (inode_tree_find_max(disk, inode_tree, root_block, &max_inode)) {
    s_next_inode_number = max_inode.inode_number + 1;
  } else {
    s_next_inode_number = 1;
  }
  s_cached_inode_root = root_block;
}

/**
 * @brief Reads an inode from the inode B-tree.
 *
 * @param disk Pointer to the open filesystem image.
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode_number The unique identifier of the inode to read.
 * @param out_inode Pointer to the inode_t structure to store the result.
 * @return 1 if the inode was found, 0 otherwise.
 */
int read_inode(FILE *disk, btree_handle_t *handle, uint32_t inode_number,
               inode_t *out_inode) {
  if (!handle || !out_inode)
    return 0;

  return btree_search(disk, handle, handle->root_block, inode_number,
                      out_inode);
}

/**
 * @brief Inserts a new inode into the inode B-tree.
 *
 * @param disk Pointer to the open filesystem image.
 * @param sb Pointer to the superblock (for block allocation).
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode Pointer to the inode to be inserted.
 */
int insert_inode(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                 const inode_t *inode) {
  if (!handle || !inode)
    return -1;

  inode_t copy;
  memcpy(&copy, inode, sizeof(inode_t));
  int rc = btree_insert(disk, sb, handle, &copy);
  /* Persist inode root whenever it changes (e.g., after a split). */
  if (rc == 0 && handle->root_block != 0 &&
      sb->inode_root != handle->root_block) {
    sb->inode_root = handle->root_block;
    (void)write_superblock(disk, sb);
  }
  if (rc == 0) {
    if (inode->inode_number >= s_next_inode_number)
      s_next_inode_number = inode->inode_number + 1;
    s_cached_inode_root = handle->root_block;
  }
  return rc;
}

/**
 * @brief Updates an existing inode in the B-tree.
 *
 * @param disk Pointer to the open filesystem image.
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode Pointer to the modified inode to be written back.
 * @return 1 if the inode was updated, 0 if not found.
 */
int update_inode(FILE *disk, btree_handle_t *handle, const inode_t *inode) {
  if (!handle || !inode)
    return -1;
  int ok = btree_update(disk, handle, handle->root_block, (void *)inode);
  return ok;
}

/**
 * @brief Removes an inode from the inode B-tree.
 *
 * This function removes the inode entry with the specified inode number
 * from the B-tree structure.
 *
 * @param disk Pointer to the open filesystem image.
 * @param sb Pointer to the superblock (needed for possible deallocations).
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode_number The inode number to be removed.
 * @return 1 if the inode was found and removed, 0 otherwise.
 */
int remove_inode(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                 uint32_t inode_number) {
  if (!handle || handle->type != BTREE_TYPE_INODE)
    return -1;
  int removed = btree_remove(disk, sb, handle, inode_number);
  if (!removed && handle) {
    inode_t tmp;
    int present =
        btree_search(disk, handle, handle->root_block, inode_number, &tmp);
    if (present) {
      if (btree_remove_entry(disk, sb, handle, &tmp)) {
        removed = 1;
      }
    }
    if (!removed) {
      fprintf(stderr,
              "remove_inode: failed to purge ino=%u (root=%u sb_root=%u present=%d)\n",
              inode_number, handle->root_block, sb ? sb->inode_root : 0,
              present);
    }
  }
  if (removed && sb->inode_root != handle->root_block) {
    sb->inode_root = handle->root_block;
    (void)write_superblock(disk, sb);
  }
  if (removed)
    inode_sync_next(disk, sb, handle);
  return removed;
}

/**
 * @brief Finds the next available inode number by scanning the inode B-tree.
 *
 * This function scans the inode tree to find the highest inode number in use
 * and returns the next available one. This ensures uniqueness of inode numbers
 * without reusing deleted ones.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the loaded superblock (used to sync metadata changes).
 * @param inode_tree Pointer to the B-tree handle for inodes.
 * @return The next available (unused) inode number.
 */
uint32_t get_next_inode_number(FILE *disk, superblock_t *sb,
                               btree_handle_t *inode_tree) {
  if (!sb)
    return s_next_inode_number++;

  inode_sync_next(disk, sb, inode_tree);

  if (s_next_inode_number == 0)
    s_next_inode_number = 1;

  return s_next_inode_number++;
}

/**
 * @brief Checks if an inode with the given number exists in the inode B-tree.
 *
 * This function performs a lookup in the inode B-tree to determine whether
 * an inode with the specified number is currently present in the file system.
 *
 * @param disk Pointer to the file representing the disk.
 * @param inode_tree Pointer to the B-tree handle managing inodes.
 * @param inode_number The inode number to check for existence.
 * @return 1 if the inode exists, 0 if it does not.
 */
int inode_exists(FILE *disk, btree_handle_t *inode_tree,
                 uint32_t inode_number) {
  inode_t temp;
  return btree_search(disk, inode_tree, inode_tree->root_block, inode_number,
                      &temp);
}

/**
 * @brief Initialize (or load) the inode B-tree handle.
 *
 * If the inode root does not exist yet in the superblock, this function
 * allocates a fresh root node on disk (empty leaf), persists it, and stores the
 * block number in both the handle and the superblock (via write_superblock).
 * If the root already exists, the handle is configured to point to it.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (may be updated/persisted if creating root).
 * @param out  Output: initialized inode B-tree handle (by value).
 * @return 1 on success, 0 on failure.
 */
int inode_tree_init(FILE *disk, superblock_t *sb, btree_handle_t *out) {
  if (!disk || !sb || !out)
    return 0;

  btree_handle_t *tmp = btree_create_inode_handle();
  if (!tmp)
    return 0;

  if (sb->inode_root == 0) {
    // Create empty root leaf node on disk
    btree_node_t node;
    node.is_leaf = 1;
    node.num_entries = 0;
    node.entries =
        calloc((size_t)(2 * tmp->min_degree - 1), (size_t)tmp->entry_size);
    node.children = calloc((size_t)(2 * tmp->min_degree), sizeof(uint32_t));
    if (!node.entries || !node.children) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }

    uint32_t root_block = allocate_single_block(disk, sb);
    if (root_block == INVALID_BLOCK || root_block == 0) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }

    tmp->root_block = root_block;
    if (btree_write_node(disk, tmp, root_block, &node) != 0) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }
    free(node.entries);
    free(node.children);

    sb->inode_root = root_block;
    if (write_superblock(disk, sb) != 0) {
      free(tmp);
      return 0;
    }
  } else {
    tmp->root_block = sb->inode_root;
  }

  *out = *tmp;
  free(tmp);
  return 1;
}
