/**
 * @file dirent.h
 * @brief Directory entries: adding, removing and looking up names.
 *
 * A directory's entries live in the bsfs_dentry tree at its btree_root, ordered
 * by (hash_filename(name), name). The parent is always d_inode(dentry->d_parent).
 * These helpers take no locks. The VFS holds the parent's i_rwsem around every
 * caller.
 */
#ifndef DIRECTORY_OPERATIONS
#define DIRECTORY_OPERATIONS

#include "../bsfs.h"

/**
 * @brief Adds @p dentry's name to its parent, pointing at @p inode.
 *
 * On success, stores the parent's new btree_root, bumps its mtime and ctime,
 * and marks it dirty. Link counts are the caller's job.
 *
 * @return 0, -EEXIST, -ENAMETOOLONG, -ENOMEM, a lookup error, or -ENOSPC for
 *         any insert failure.
 */
int bsfs_add_link(struct dentry *dentry, struct inode *inode);

/**
 * @brief Removes @p dentry's name from its parent.
 *
 * Updates the parent like bsfs_add_link() does. Allocates at BSFS_ALLOC_SHRINK
 * so unlink can still run on a full disk. Leaves the target inode's nlink
 * alone.
 *
 * @return 0, -ENOENT, -ENOMEM, a lookup error, or -EIO if the removal failed.
 */
int bsfs_remove_link(struct dentry *dentry);

/**
 * @brief Looks up a directory entry by name in a given directory.
 *
 * Descends the entry tree by (hash, name) rather than scanning it, so this is
 * logarithmic in the size of the directory. The entry is copied out. Returning
 * a pointer into a node's buffer is a trap under copy-on-write, where the next
 * mutation clones that node and releases the block it came from.
 *
 * @param dentry The dentry whose name is looked up in its parent.
 * @param out Filled with the matching entry when the return value is 1.
 * @return 1 if found, 0 if not, -ENAMETOOLONG, -ENOMEM, or a read error.
 */
int lookup_dir_entry(struct dentry *dentry, struct bsfs_dentry *out);

#endif // !DIRECTORY_OPERATIONS
