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
} dir_t;
#endif // !DIRECTORY_OPERATIONS
