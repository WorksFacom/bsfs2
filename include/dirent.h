/**
 * @file dir_entry_operations_bsfs.h
 * @brief Directory entry B-tree operations for BSFS (public interface).
 *
 * Provides the in-memory directory descriptor (@ref dir_t) and the set of
 * operations for creating, removing, updating, looking up, and iterating
 * directory entries backed by a B-tree stored in the filesystem image.
 */
#ifndef DIRECTORY_OPERATIONS
#define DIRECTORY_OPERATIONS

#include "../bsfs.h"

/**
 * @brief Adds a new entry to a directory.
 *
 * Inserts a new @ref dir_entry_t into the directory's B-tree. If this is the
 * first insertion in the directory (i.e., the directory had no B-tree root),
 * the function persists the newly created root into the directory inode.
 *
 * @param disk Pointer to the filesystem image file.
 * @param sb Pointer to the loaded superblock.
 * @param dir Pointer to the open directory structure.
 * @param entry Pointer to the directory entry to insert.
 * @return 0 on success, -1 if an entry with the same name already exists or on
 *         write error.
 */
int bsfs_add_link(struct dentry *dentry, struct inode *inode);

/**
 * @brief Removes an entry from a directory by name.
 *
 * Looks up the entry in the B-tree and removes it if found.
 *
 * @param disk Pointer to the filesystem image file.
 * @param sb Pointer to the loaded superblock.
 * @param dir Pointer to the open directory structure.
 * @param filename Name of the entry to remove.
 * @return 0 on success, -1 if the entry was not found or on write error.
 */
int bsfs_remove_link(struct dentry *dentry);

/**
 * @brief Looks up a directory entry by name in a given directory.
 *
 * Descends the entry B-tree by (hash, name) rather than scanning it, so this is
 * logarithmic in the size of the directory. The entry is copied out: returning a
 * pointer into a node's buffer is a trap under copy-on-write, where the next
 * mutation clones that node and releases the block it came from.
 *
 * @param dentry The dentry whose name is looked up in its parent.
 * @param out Filled with the matching entry when the return value is 1.
 * @return 1 if found, 0 if not, a negative error on I/O failure.
 */
int lookup_dir_entry(struct dentry *dentry, struct bsfs_dentry *out);

#endif // !DIRECTORY_OPERATIONS
