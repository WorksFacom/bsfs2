/**
 * @file btree_bsfs.h
 * @brief Public interface for the B-tree implementation in the BSFS filesystem.
 *
 * This header defines the core structures and function prototypes for handling
 * B-tree operations over disk blocks in the BSFS system. B-trees are used to
 * organize entries such as inodes, block spans (bspan), and recovery entries.
 *
 * The API supports creation, insertion, search, update, and removal of entries
 * in the B-tree stored on disk, and allows configuration for different types of
 * data.
 */
#ifndef BTREE_BSFS_H
#define BTREE_BSFS_H

#include "allocate_bsfs.h"
#include "block_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include <stdint.h>
#include <stdio.h>

/**
 * @enum btree_data_type_t
 * @brief Represents the type of data stored in the B-tree.
 *
 * This enumeration defines the supported logical types of data that can be
 * stored in B-trees managed by the BSFS filesystem. Each type corresponds to a
 * different kind of entry managed in the system.
 */
typedef enum {
  BTREE_TYPE_INODE,
  BTREE_TYPE_BSPAN,
  BTREE_TYPE_RECOVERY
} btree_data_type_t;

/**
 * @typedef key_fn
 * @brief Function pointer type for extracting a key from an entry.
 *
 * A function of this type takes a pointer to an entry and returns a unique
 * 32-bit key used to index and compare entries within the B-tree.
 */
typedef uint32_t (*key_fn)(const void *entry);

/**
 * @typedef compare_fn
 * @brief Function pointer type for comparing two entries.
 *
 * A comparison function used to determine the ordering of two entries in
 * the B-tree. It returns a negative value if a < b, zero if a == b,
 * and a positive value if a > b.
 */
typedef int (*compare_fn)(const void *a, const void *b);

/**
 * @struct btree_node_t
 * @brief Represents a single node in the B-tree stored on disk.
 *
 * This structure holds metadata and content for a B-tree node.
 * It supports both leaf and internal nodes, with a flexible entry format
 * configured via the B-tree handle (`btree_handle_t`).
 *
 * - If `is_leaf` is 1, the node does not have child pointers.
 * - If `is_leaf` is 0, the `children` array contains block numbers pointing to
 * child nodes.
 *
 * The number of entries and children is determined by the minimum degree `t`:
 * - Maximum entries: `2t - 1`
 * - Maximum children: `2t`
 */
typedef struct {
  /**
   * Flag indicating whether the node is a leaf (1) or internal node (0).
   */
  uint8_t is_leaf;

  /**
   * Number of valid entries currently stored in the node.
   */
  uint32_t num_entries;

  /**
   * Pointer to the array of entries (opaque key/data pairs).
   * The format and size of each entry are defined in the B-tree handle.
   */
  void *entries;

  /**
   * Pointer to the array of child block numbers (only used if the node is
   * internal). The array can hold up to `2t` block addresses.
   */
  uint32_t *children;
} btree_node_t;

/**
 * @struct btree_handle_t
 * @brief Describes the configuration and root reference for a B-tree instance.
 *
 * This structure holds all metadata and function pointers necessary for
 * managing a particular instance of a B-tree. It allows the B-tree
 * implementation to be generic and operate on arbitrary data types, defined via
 * the handlers below.
 *
 */
typedef struct {
  /**
   * Type of data this B-tree stores.
   */
  btree_data_type_t type;

  /**
   * Entry size: Size of each element (in bytes).
   */
  size_t entry_size;

  /**
   * Function to compare two entries.
   */
  compare_fn cmp;

  /**
   * Function to extract the key from an entry.
   */
  key_fn get_key;

  /**
   * Block number of the root node on disk.
   */
  uint32_t root_block;

  /**
   * Minimum degree `t` used to size the tree.
   */
  int min_degree;
} btree_handle_t;

/**
 * @brief Creates a generic B-tree handle for managing structured data.
 *
 * This function initializes a `btree_handle_t` structure, which serves as the
 * context for performing B-tree operations (insertion, search, removal, etc.)
 * on a specific data type. It sets up key parameters including:
 * - The type of data being stored (`btree_data_type_t`)
 * - The size of each entry (in bytes)
 * - A comparison function for ordering keys
 * - A key extraction function used by the B-tree logic
 *
 * If a comparison function is not provided, the default one (`default_compare`)
 * is used. The function also calculates the minimum degree of the B-tree nodes
 * based on the entry size and `BLOCK_SIZE`.
 *
 * @param type The type of data the B-tree will manage (e.g., INODE, BSPAN,
 * RECOVERY).
 * @param entry_size The size in bytes of each entry in the tree.
 * @param cmp A comparison function for comparing two entries. Can be NULL.
 * @param get_key A function to extract the key from an entry.
 * @return Pointer to a newly allocated and configured `btree_handle_t`.
 */
btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size,
                                    compare_fn cmp, key_fn get_key);

/**
 * @brief Creates a B-tree handle specifically for managing inode entries.
 *
 * This function initializes and returns a pointer to a `btree_handle_t`
 * configured to operate on entries of type `inode_t`. It sets the appropriate:
 * - Data type identifier (`BTREE_TYPE_INODE`)
 * - Entry size (based on `sizeof(inode_t)`)
 * - Key extraction function (`get_inode_key`)
 * - Comparison function (`compare_inode`)
 *
 * The resulting handle can be used for all B-tree operations involving inodes,
 * such as insertion, search, update, and removal.
 *
 * @return Pointer to a newly allocated and configured `btree_handle_t` for
 * inodes.
 */
btree_handle_t *btree_create_inode_handle(void);

/**
 * @brief Creates a B-tree handle for managing bspan entries.
 *
 * This function initializes a `btree_handle_t` configured for use with
 * `bspan_t` entries. It sets:
 * - The data type identifier to `BTREE_TYPE_BSPAN`
 * - The entry size to `sizeof(bspan_t)`
 * - The comparison function to `compare_bspan`
 * - The key extraction function to `get_bspan_key`
 *
 * This handle can be used for managing logical-to-physical block mappings
 * of files stored in the BSFS.
 *
 * @return Pointer to a configured `btree_handle_t` for bspan entries.
 */
btree_handle_t *btree_create_bspan_handle(void);

/**
 * @brief Creates a B-tree handle for managing recovery entries.
 *
 * This function sets up a `btree_handle_t` for use with entries of type
 * `rec_entry_t`. It specifies:
 * - The data type as `BTREE_TYPE_RECOVERY`
 * - The entry size as `sizeof(rec_entry_t)`
 * - The comparison function as `compare_rec_entry`
 * - The key extraction function as `get_rec_entry_key`
 *
 * The resulting handle is used to manage entries in the recovery subsystem
 * of the BSFS.
 *
 * @return Pointer to a configured `btree_handle_t` for recovery entries.
 */
btree_handle_t *btree_create_rec_entry_handle(void);

/**
 * @brief Inserts a new entry into the B-tree.
 *
 * This function inserts a new entry into the B-tree associated with the given
 * `btree_handle_t`. If the tree is empty, it initializes the root node.
 * If the root node is full, it performs a split and creates a new root,
 * maintaining the B-tree balance properties. Otherwise, it inserts directly
 * into the appropriate non-full node.
 *
 * Internally, the function may allocate new nodes on disk using the
 * `allocate_single_block` function and uses helper functions
 * `btree_insert_nonfull` and `btree_split_child` to perform recursive
 * insertion and node splitting logic.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock structure, used for block allocation.
 * @param handle Pointer to the B-tree handle containing metadata and logic
 * functions.
 * @param entry Pointer to the entry to be inserted (must match the tree's entry
 * type).
 */
void btree_insert(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                  void *entry);

/**
 * @brief Searches for a key in the B-tree and retrieves the associated entry.
 *
 * This function performs a recursive search in the B-tree starting at the node
 * identified by `node_block`. It uses the comparison function and key extractor
 * provided in the B-tree handle to locate the entry corresponding to the given
 * key.
 *
 * If the key is found, the corresponding entry is copied into `output`.
 * If not found, the function returns 0.
 *
 * @param disk Pointer to the open disk file.
 * @param handle Pointer to the B-tree handle containing type and logic
 * information.
 * @param node_block Block number of the node to begin searching from.
 * @param key The key to search for.
 * @param output Pointer to a memory location where the found entry will be
 * copied.
 * @return int 1 if the key was found, 0 otherwise.
 */
int btree_search(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 uint32_t key, void *output);

/**
 * @brief Updates an existing entry in the B-tree with new data.
 *
 * This function searches for an entry in the B-tree based on the key extracted
 * from `new_entry`, and if found, replaces the existing entry with the new one.
 * The update is performed in-place within the node and persisted to disk.
 *
 * If the key is not found in the current node and the node is not a leaf,
 * the function recurses into the appropriate child. If the key is not found
 * anywhere in the tree, the function returns 0 and no update occurs.
 *
 * @param disk Pointer to the open disk file.
 * @param handle Pointer to the B-tree handle that contains type-specific
 * information.
 * @param node_block Block number of the node to start searching from (typically
 * the root).
 * @param new_entry Pointer to the entry containing updated data (must contain a
 * valid key).
 * @return int 1 if the entry was found and updated, 0 if the key was not found.
 */
int btree_update(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 void *new_entry);

/**
 * @brief Removes an entry from the B-tree based on its key.
 *
 * This is the public interface for removing an entry from a B-tree. It
 * initiates a recursive deletion process starting from the node specified by
 * `node_block`.
 *
 * Internally, the actual logic is handled by `btree_remove_internal`, which
 * updates the tree structure, balances nodes if necessary, and writes changes
 * to disk.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock, used for block deallocation if
 * necessary.
 * @param handle Pointer to the B-tree handle structure.
 * @param node_block The block number of the root (or subroot) node to start the
 * removal.
 * @param key The key of the entry to remove from the tree.
 * @return int 1 if the key was found and removed, 0 if the key was not found.
 */
int btree_remove(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                 uint32_t node_block, uint32_t key);

/**
 * @brief Serializes and writes a B-tree node to disk.
 *
 * This function converts a B-tree node structure into a binary format
 * and writes it to a specified disk block. The serialization includes:
 * - A flag indicating whether the node is a leaf.
 * - The number of entries in the node.
 * - The array of entries (`entry_size` * (2t - 1)).
 * - The array of child block pointers (2t).
 *
 * The total serialized size must not exceed `BLOCK_SIZE`, and the layout
 * must be consistent with `btree_read_node` to ensure correct deserialization.
 *
 * @param disk Pointer to the opened disk file.
 * @param handle Pointer to the B-tree handle (used to determine entry size and
 * degree).
 * @param block The disk block number where the node will be written.
 * @param node Pointer to the B-tree node to serialize and write.
 */
void btree_write_node(FILE *disk, btree_handle_t *handle, uint32_t block,
                      btree_node_t *node);

/**
 * @brief Reads and deserializes a B-tree node from disk.
 *
 * This function reads a B-tree node from the specified disk block and
 * reconstructs it into a `btree_node_t` structure in memory. The format
 * must match the layout written by `btree_write_node`, including:
 * - A flag indicating whether the node is a leaf.
 * - The number of entries.
 * - The array of entries.
 * - The array of child pointers.
 *
 * Memory for the node and its arrays is dynamically allocated.
 * The caller is responsible for freeing the resulting node using
 * `btree_free_node`.
 *
 * @param disk Pointer to the opened disk file.
 * @param handle Pointer to the B-tree handle (used to determine entry size and
 * degree).
 * @param block The disk block number to read from.
 * @return Pointer to a newly allocated and populated `btree_node_t` structure.
 */
btree_node_t *btree_read_node(FILE *disk, btree_handle_t *handle,
                              uint32_t block);

/**
 * @brief Frees the memory allocated for a B-tree node.
 *
 * This function deallocates all dynamic memory used by a `btree_node_t`
 * structure, including:
 * - The entries array.
 * - The children array.
 * - The node structure itself.
 *
 * Should be called after reading or allocating a B-tree node to avoid memory
 * leaks.
 *
 * @param node Pointer to the B-tree node to be deallocated.
 */
void btree_free_node(btree_node_t *node);

/**
 * @brief Recursively deallocates all nodes and data blocks in a B-tree.
 *
 * This function traverses the entire B-tree starting from the given node block.
 * It recursively frees child nodes (if any), and also deallocates each data
 * block referenced by bspan_t entries (only applies to BTREE_TYPE_BSPAN).
 * Finally, it deallocates the node block itself from the bitmap.
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param handle Pointer to the B-tree handle.
 * @param node_block Block number of the current node.
 */
void btree_deallocate_all(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                          uint32_t node_block);

#endif // BTREE_BSFS_H
