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
 * @struct dir_t
 * @brief Represents an open directory in memory in the BSFS file system.
 *
 * This structure is used to manage the iteration and manipulation of
 * directory entries during read operations like readdir().
 */
// typedef struct {
//   bsfs_inode      inode;         ///< The inode of the directory
//   uint32_t        current_index; ///< The logical position of the next entry to read
//   btree_handle_t *btree;         ///< Handle to the B-tree storing directory entries
//   /* Iteration state for in-order traversal */
//   uint32_t iter_key;     ///< Last returned key (filename hash)
//   int      iter_started; ///< 0 if iteration not started, 1 otherwise
// } dir_t;

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
 * @brief Updates an existing directory entry in the B-tree.
 *
 * Searches for an existing directory entry using the hash of the filename and
 * replaces its contents with @p entry.
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock (unused).
 * @param dir Pointer to the open directory structure.
 * @param entry Pointer to the updated directory entry.
 * @return 1 on success; -1 if the entry was not found.
 */
// int update_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, dir_entry_t *entry);

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

/**
 * @brief Reads the next directory entry from a directory.
 *
 * This function traverses the B-tree of a directory and returns the next valid
 * entry. It uses the `dir_index` field to keep track of position across calls.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock.
 * @param dir Pointer to the open directory structure (dir_t).
 * @param handle B-tree handler for the directory entries.
 * @param out Output parameter to hold the directory entry read.
 * @return 0 if an entry was read successfully, -1 if end of directory is
 * reached.
 */
// int read_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, btree_handle_t *handle, dir_entry_t *out);

#endif // !DIRECTORY_OPERATIONS
