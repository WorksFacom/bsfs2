/**
 * @file inode.h
 * @brief Inodes: the inode tree, the VFS inode lifecycle, and block mapping.
 *
 * Every inode is a disk_mapped_inode_t in the inode tree, whose root sits in
 * super->inode_root. sbi->inode_tree_lock serializes all access to that tree.
 * Each file's own extents or dentries hang off its btree_root, and
 * bsfs_inode.truncate_mutex guards that tree.
 */
#ifndef INODE_OPERATIONS_H
#define INODE_OPERATIONS_H

#include "../bsfs.h"
#include "btree.h"

/** @brief Buffer-head address space operations, shared by files, dirs and symlinks. */
extern const struct address_space_operations bsfs_aops;

/**
 * @brief Inserts @p inode into the inode tree and publishes the new root.
 *
 * Writes super->inode_root and dirties the superblock buffer. The caller holds
 * sbi->inode_tree_lock.
 *
 * @return 0, or -1 if the insert failed.
 */
int insert_inode(struct super_block *sb, btree_handle_t *handle, const struct disk_mapped_inode_t *inode);

/**
 * @brief Returns the highest inode number in use plus one.
 *
 * Walks the rightmost path of the tree on every call, so the number of an
 * evicted inode comes back if it was the highest. The caller holds
 * sbi->inode_tree_lock.
 *
 * @return the next inode number, 1 for an empty tree, or 0 if the walk failed.
 */
__u32 get_next_inode_number(struct super_block *sb, btree_handle_t *inode_tree);

/**
 * @brief Installs the operation tables that match inode->i_mode.
 *
 * Special files go through init_special_inode() with @p rdev. An unknown type
 * makes the inode bad.
 */
void bsfs_set_inode(struct inode *inode, dev_t rdev);

/**
 * @brief iget for bsfs. Returns the cached inode or reads it from the inode tree.
 *
 * @return the inode, or ERR_PTR(-ENOMEM), ERR_PTR(-EIO) if @p ino is not in the
 *         tree, or ERR_PTR(-ESTALE) if it is there with nlinks 0.
 */
struct inode *bsfs_get_vfs_inode(struct super_block *sb, unsigned long ino);

/** @brief ->alloc_inode. Takes a bsfs_inode from the slab cache, or returns NULL. */
struct inode *bsfs_alloc_inode(struct super_block *sb);

/**
 * @brief Finds @p ino in the inode tree and returns it still pinned.
 *
 * The returned wrapper's ->inode points into ->bh. The caller must brelse(->bh)
 * and kfree() the wrapper, and holds sbi->inode_tree_lock while reading
 * through ->inode, since any mutation of the tree can move the entry.
 *
 * @return the wrapper, or NULL if @p ino is missing or anything failed.
 */
struct disk_mapped_inode *bsfs_read_disk_inode(struct super_block *sb, ino_t ino);

/**
 * @brief Cuts the extent tree down to i_size.
 *
 * Call after i_size is set. Trims the span that straddles the cut and removes
 * every span past it. The released blocks are deferred, not freed. Allocates
 * at BSFS_ALLOC_SHRINK under truncate_mutex. Failures are only logged, since
 * ->setattr has already committed to the new size by then.
 */
void bsfs_truncate(struct inode *inode);

/**
 * @brief ->write_inode. Copies the VFS inode into its inode tree entry.
 *
 * A copy-on-write update at BSFS_ALLOC_WRITEBACK, which may dig into half the
 * reserve because publishing roots is what lets deferred blocks go. Errors
 * are recorded in sbi->writeback_error so the next commit barrier fails
 * instead of freeing blocks the disk still needs.
 *
 * @return 0, -ENOMEM or -EIO.
 */
int bsfs_write_inode(struct inode *inode, struct writeback_control *wbc);

/**
 * @brief Allocates an inode number and inserts a blank entry for it.
 *
 * The inode comes back hashed, dirty, with an empty btree_root and owner set
 * from @p dir. The caller still installs the ops with bsfs_set_inode().
 *
 * @return the inode, ERR_PTR(-ENOMEM), or ERR_PTR(-ENOSPC) if no number or no
 *         block was available.
 */
struct inode *bsfs_new_inode(const struct inode *dir, umode_t mode);

/** @brief ->free_inode. Returns the bsfs_inode to the slab cache. */
void bsfs_free_cache_inode(struct inode *inode);

/**
 * @brief ->evict_inode. Deletes the inode if its last link is gone.
 *
 * An inode dropped to nlink 0 by unlink, rmdir or an overwriting rename becomes
 * a recovery snapshot, which takes over its tree so the data stays on disk. If
 * the snapshot cannot be recorded, or the inode got to nlink 0 some other way,
 * its tree is deferred with btree_deallocate_all(). Either way its entry leaves
 * the inode tree.
 */
void bsfs_evict_inode(struct inode *inode);

/** @brief Creates the bsfs_inode slab cache at module load. 0 or -ENOMEM. */
int __init init_inodecache(void);

/** @brief Destroys the slab cache at module unload, after an RCU barrier. */
void destroy_inodecache(void);

/**
 * @brief get_block_t for the buffer-head helpers.
 *
 * Maps @p block through the extent tree. With @p create set, a hole gets a fresh
 * block. If that block lands right after the previous span on disk, the span
 * grows instead of a new one being inserted. Takes truncate_mutex.
 *
 * @return 0, -ENOSPC, -EIO, -ENOMEM, or a read error.
 */
int bsfs_get_block(struct inode *inode, sector_t block, struct buffer_head *bh, int create);

#endif // INODE_OPERATIONS_H
