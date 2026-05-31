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
// int add_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, dir_entry_t *entry);

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
// int remove_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, const char *filename);

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
 * @param disk Pointer to the disk image.
 * @param sb Pointer to the superblock.
 * @param dir Pointer to the open directory (dir_t).
 * @param name Name of the entry to look for.
 * @param result Output parameter: filled with the entry if found.
 * @return 0 if found, -1 if not found.
 */
struct bsfs_dentry *lookup_dir_entry(struct dentry *dentry, struct btree_node **out_node);

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

/**
 * @brief Recursively search a directory B-tree for an exact filename match.
 *
 * Traverses the directory entries B-tree in depth-first order starting at
 * @p node_block, comparing each entry's name against @p name. Copies the
 * first matching entry into @p out and returns 1 when found.
 *
 * @param disk Pointer to the open disk file.
 * @param handle B-tree handle for directory entries.
 * @param node_block Root block of the subtree to scan.
 * @param name Null-terminated filename to match exactly.
 * @param out Output parameter to receive the matching entry on success.
 * @return 1 if found; 0 otherwise.
 */
int __scan_dir_tree_find(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const char *name, struct bsfs_dentry **output,
                         struct btree_node **out_node);

int bsfs_add_link(struct dentry *dentry, struct inode *inode);

#endif // !DIRECTORY_OPERATIONS
