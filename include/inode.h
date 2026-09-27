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

#include "../bsfs.h"
#include "btree.h"

extern const struct address_space_operations bsfs_aops;

/**
 * @brief Inserts a new inode into the inode B-tree.
 *
 * @param sb Pointer to the super block (for block allocation).
 * @param handle Pointer to the B-tree handle for inode management.
 * @param inode Pointer to the inode to be inserted.
 */
int insert_inode(struct super_block *sb, btree_handle_t *handle, const struct disk_mapped_inode_t *inode);

/**
 * @brief Finds the next available inode number by scanning the inode B-tree.
 *
 * This function scans the inode tree to find the highest inode number in use
 * and returns the next available one. This ensures uniqueness of inode numbers
 * without reusing deleted ones.
 *
 * @param inode_tree Pointer to the B-tree handle for inodes.
 * @return The next available (unused) inode number.
 */
__u32 get_next_inode_number(struct super_block *sb, btree_handle_t *inode_tree);

void bsfs_set_inode(struct inode *inode, dev_t rdev);

struct inode *bsfs_get_vfs_inode(struct super_block *sb, unsigned long ino);

struct inode *bsfs_alloc_inode(struct super_block *sb);

struct disk_mapped_inode *bsfs_read_disk_inode(struct super_block *sb, ino_t ino);

void bsfs_truncate(struct inode *inode);

int bsfs_write_inode(struct inode *inode, struct writeback_control *wbc);

struct inode *bsfs_new_inode(const struct inode *dir, umode_t mode);

void bsfs_free_cache_inode(struct inode *inode);

void bsfs_evict_inode(struct inode *inode);

int __init init_inodecache(void);

void destroy_inodecache(void);

int bsfs_get_block(struct inode *inode, sector_t block, struct buffer_head *bh, int create);

#endif // INODE_OPERATIONS_H
