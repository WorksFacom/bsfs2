#ifndef DIRECTORY_OPERATIONS
#define DIRECTORY_OPERATIONS

#include "btree_bsfs.h"
#include "filesystem_bsfs.h"
#include "inode_operations_bsfs.h"

/**
 * @struct dir_t
 * @brief Represents an open directory in memory in the BSFS file system.
 *
 * This structure is used to manage the iteration and manipulation of
 * directory entries during read operations like readdir().
 */
typedef struct {
  inode_t inode;          ///< The inode of the directory
  uint32_t current_index; ///< The logical position of the next entry to read
  btree_handle_t *btree;  ///< Handle to the B-tree storing directory entries
  /* Iteration state for in-order traversal */
  uint32_t iter_key;      ///< Last returned key (filename hash)
  int iter_started;       ///< 0 if iteration not started, 1 otherwise
} dir_t;

/**
 * @brief Opens a directory given its inode number.
 *
 * Loads the directory inode from the inode B-tree, initializes a dir_t
 * structure and prepares the B-tree handle for reading directory entries.
 *
 * @param disk Pointer to the open filesystem image.
 * @param inode_tree Pointer to the B-tree containing inodes.
 * @param inode_number The inode number of the directory to open.
 * @return Pointer to an allocated dir_t on success, NULL on failure.
 */
dir_t *open_dir(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number);

/**
 * @brief Closes a directory and frees allocated resources.
 *
 * This function deallocates the memory used by the dir_t structure
 * and its associated B-tree handle.
 *
 * @param dir Pointer to the open directory to be closed.
 * @return 0 on success.
 */
int close_dir(dir_t *dir);

/**
 * @brief Adds a new entry to a directory.
 *
 * This function inserts a new dir_entry_t into the directory's B-tree
 * structure.
 *
 * @param disk Pointer to the filesystem image file.
 * @param sb Pointer to the loaded superblock.
 * @param dir Pointer to the open directory structure.
 * @param entry Pointer to the directory entry to insert.
 * @return 0 on success, -1 if an entry with the same name already exists.
 */
int add_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir, dir_entry_t *entry);

/**
 * @brief Removes an entry from a directory by name.
 *
 * Looks up the entry in the B-tree and removes it if found.
 *
 * @param disk Pointer to the filesystem image file.
 * @param sb Pointer to the loaded superblock.
 * @param dir Pointer to the open directory structure.
 * @param filename Name of the entry to remove.
 * @return 0 on success, -1 if the entry was not found.
 */
int remove_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                     const char *filename);

/**
 * @brief Updates an existing directory entry in the B-tree.
 *
 * This function searches for an existing directory entry using the hash of the
 * filename and replaces its contents with the new data.
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param dir Pointer to the open directory structure.
 * @param entry Pointer to the updated directory entry.
 * @return 0 on success, -1 if the entry was not found.
 */
int update_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                     dir_entry_t *entry);

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
int lookup_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir, const char *name,
                     dir_entry_t *result);

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
int read_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                   btree_handle_t *handle, dir_entry_t *out);

#endif // !DIRECTORY_OPERATIONS
