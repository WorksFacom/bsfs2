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
#include "../../include/inode_operations_bsfs.h"

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
  return btree_insert(disk, sb, handle, &copy);
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

  return btree_update(disk, handle, handle->root_block, (void *)inode);
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
  return btree_remove(disk, sb, handle, inode_number);
}

/**
 * @brief Finds the next available inode number by scanning the inode B-tree.
 *
 * This function scans the inode tree to find the highest inode number in use
 * and returns the next available one. This ensures uniqueness of inode numbers
 * without reusing deleted ones.
 *
 * @param disk Pointer to the disk file.
 * @param inode_tree Pointer to the B-tree handle for inodes.
 * @return The next available (unused) inode number.
 */
uint32_t get_next_inode_number(FILE *disk, btree_handle_t *inode_tree) {
  if (!inode_tree || inode_tree->root_block == 0)
    return 1;

  uint32_t max_inode = 0;
  btree_node_t *node =
      btree_read_node(disk, inode_tree, inode_tree->root_block);

  while (!node->is_leaf) {
    uint32_t last_child = node->children[node->num_entries];
    btree_free_node(node);
    node = btree_read_node(disk, inode_tree, last_child);
  }

  if (node->num_entries > 0) {
    inode_t *entries = (inode_t *)node->entries;
    inode_t *last_inode = &entries[node->num_entries - 1];
    max_inode = last_inode->inode_number;
  }

  btree_free_node(node);
  return max_inode + 1;
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
