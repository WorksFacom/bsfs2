/**
 * @file inode_operations_bsfs.h
 * @brief Interface for inode management operations in the BSFS file system.
 *
 * This header defines functions for manipulating inodes in the BSFS file
 * system. It provides operations to insert, read, update, and delete inodes
 * within the B-tree structure dedicated to inode storage.
 *
 * These operations abstract the interaction with the underlying B-tree and
 * ensure that inode access remains consistent, modular, and type-safe.
 */
#ifndef INODE_OPERATIONS_H
#define INODE_OPERATIONS_H

#include <stdio.h>

#include "btree.h"
#include "fs.h"

/**
 * @brief Reads an inode from the inode B-tree.
 *
 * @param disk Pointer to the open filesystem image.
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode_number The unique identifier of the inode to read.
 * @param out_inode Pointer to the inode_t structure to store the result.
 * @return 1 if the inode was found, 0 otherwise.
 */
int read_inode(FILE *disk, btree_handle_t *handle, uint32_t inode_number, inode_t *out_inode);

/**
 * @brief Inserts a new inode into the inode B-tree.
 *
 * @param disk Pointer to the open filesystem image.
 * @param sb Pointer to the superblock (for block allocation).
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode Pointer to the inode to be inserted.
 */
int insert_inode(FILE *disk, superblock_t *sb, btree_handle_t *handle, const inode_t *inode);

/**
 * @brief Updates an existing inode in the B-tree.
 *
 * @param disk Pointer to the open filesystem image.
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode Pointer to the modified inode to be written back.
 * @return 1 if the inode was updated, 0 if not found.
 */
int update_inode(FILE *disk, btree_handle_t *handle, const inode_t *inode);

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
int remove_inode(FILE *disk, superblock_t *sb, btree_handle_t *handle, uint32_t inode_number);

/**
 * @brief Finds the next available inode number by scanning the inode B-tree.
 *
 * This function scans the inode tree to find the highest inode number in use
 * and returns the next available one. This ensures uniqueness of inode numbers
 * without reusing deleted ones.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the loaded superblock (used to persist root changes).
 * @param inode_tree Pointer to the B-tree handle for inodes.
 * @return The next available (unused) inode number.
 */
uint32_t get_next_inode_number(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree);

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
int inode_exists(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number);

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
int inode_tree_init(FILE *disk, superblock_t *sb, btree_handle_t *out);

#endif // INODE_OPERATIONS_H
