/**
 * @file btree_bsfs.c
 * @brief Implementation of B-tree operations for the BSFS filesystem.
 *
 * This file implements a disk-based B-tree used to manage mappings in the BSFS
 * filesystem, such as inode references, block span allocations, and recovery
 * indexes.
 *
 * The B-tree is parameterized by entry type and size, and each node is stored
 * in a disk block. This implementation supports search, insertion, update,
 * and deletion operations, ensuring the tree remains balanced across writes.
 */
#include "../../include/btree_bsfs.h"

/*
 * Static functions prototypes
 */
static int btree_insert_nonfull(FILE *disk, superblock_t *sb,
                                btree_handle_t *handle, uint32_t node_block,
                                btree_node_t *node, void *entry);
static int btree_split_child(FILE *disk, superblock_t *sb,
                             btree_handle_t *handle, uint32_t parent_block,
                             btree_node_t *parent, int index);
static void btree_remove_internal(FILE *disk, superblock_t *sb,
                                  btree_handle_t *handle, uint32_t node_block,
                                  uint32_t key, int *success);
static void btree_merge(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                        uint32_t parent_block, btree_node_t *parent, int idx);
static void btree_fill(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                       uint32_t parent_block, btree_node_t *parent, int idx);
static void btree_borrow_from_prev(FILE *disk, btree_handle_t *handle,
                                   uint32_t parent_block, btree_node_t *parent,
                                   int idx);
static void btree_borrow_from_next(FILE *disk, btree_handle_t *handle,
                                   uint32_t parent_block, btree_node_t *parent,
                                   int idx);
/**
 * @brief Default comparison function for integer keys (uint32_t).
 *
 * This function interprets the input pointers as pointers to uint32_t values
 * and compares them. It returns a result suitable for use in sorting or
 * search functions.
 *
 * @param a Pointer to the first value (expected to point to a uint32_t).
 * @param b Pointer to the second value (expected to point to a uint32_t).
 * @return int -1 if a < b, 1 if a > b, 0 if equal.
 */
static int default_compare(const void *a, const void *b) {
  uint32_t ka = *(uint32_t *)a;
  uint32_t kb = *(uint32_t *)b;
  return (ka < kb) ? -1 : (ka > kb);
}

/**
 * @brief Computes the minimum degree (t) of a B-tree node based on the entry
 * size.
 *
 * This function calculates the maximum value of the minimum degree `t` such
 * that a B-tree node with `(2t - 1)` entries and `(2t)` child pointers still
 * fits within the defined block size (`BLOCK_SIZE`). It considers:
 * - Total space used by entries (based on `entry_size`)
 * - Total space used by child pointers (32-bit each)
 * - Overhead for metadata (`is_leaf` and `num_entries`)
 *
 * This ensures that each B-tree node can be written to a single disk block.
 *
 * @param entry_size The size in bytes of a single entry in the B-tree.
 * @return int The maximum minimum degree `t` that fits within a block.
 */
static int compute_min_degree(size_t entry_size) {
  size_t entry_area = entry_size * (2 * 4 - 1);
  size_t pointer_area = sizeof(uint32_t) * (2 * 4);
  int t = 2;
  while (1) {
    entry_area = entry_size * (2 * t - 1);
    pointer_area = sizeof(uint32_t) * (2 * t);
    size_t node_overhead = sizeof(uint8_t) + sizeof(uint32_t);
    if (entry_area + pointer_area + node_overhead > BLOCK_SIZE) {
      return t - 1;
    }
    t++;
  }
}

/**
 * @brief Shifts B-tree entries to the left in memory.
 *
 * This function performs an in-place memory shift to remove an entry from a
 * B-tree node. It moves all entries after the `from` index one position to the
 * left, effectively overwriting the entry at position `from`.
 *
 * This is typically used after deleting an entry from a node to maintain
 * the contiguous layout of the entries array.
 *
 * @param entries Pointer to the beginning of the entries array.
 * @param entry_size The size in bytes of each entry.
 * @param from The index at which the shift begins.
 * @param count The total number of entries currently in the node.
 */
static void btree_shift_left(void *entries, size_t entry_size, int from,
                             int count) {
  for (int i = from; i < count - 1; i++) {
    memcpy((uint8_t *)entries + i * entry_size,
           (uint8_t *)entries + (i + 1) * entry_size, entry_size);
  }
}

/**
 * @brief Extracts the key from an inode entry.
 *
 * This function casts the input pointer to an `inode_t` and returns its
 * `inode_number` field, which is used as the key for insertion, search, and
 * comparison in the B-tree of inodes.
 *
 * @param entry Pointer to the inode entry (expected to be of type `inode_t`).
 * @return uint32_t The inode number used as the key.
 */
uint32_t get_inode_key(const void *entry) {
  return ((const inode_t *)entry)->inode_number;
}

/**
 * @brief Compares two inode entries by their inode number.
 *
 * This function compares the keys extracted from two `inode_t` entries
 * using `get_inode_key`. It returns:
 * - A negative value if a < b
 * - Zero if a == b
 * - A positive value if a > b
 *
 * This comparison logic is used by the B-tree to maintain order among inode
 * entries.
 *
 * @param a Pointer to the first inode entry.
 * @param b Pointer to the second inode entry.
 * @return int Comparison result: negative, zero, or positive.
 */
int compare_inode(const void *a, const void *b) {
  return (get_inode_key(a) > get_inode_key(b)) -
         (get_inode_key(a) < get_inode_key(b));
}

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
btree_handle_t *btree_create_inode_handle() {
  return btree_create_handle(BTREE_TYPE_INODE, sizeof(inode_t), compare_inode,
                             get_inode_key);
}

/**
 * @brief Extracts the key from a bspan entry.
 *
 * This function casts the input pointer to a `bspan_t` and returns the
 * `file_blk` field, which represents the logical file block number.
 * This value is used as the key in B-tree operations for bspan entries.
 *
 * @param entry Pointer to a bspan entry (expected to be of type `bspan_t`).
 * @return uint32_t The logical file block number used as the key.
 */
uint32_t get_bspan_key(const void *entry) {
  return ((const bspan_t *)entry)->file_blk;
}

/**
 * @brief Compares two bspan entries by their logical file block number.
 *
 * This function compares the keys extracted from two `bspan_t` entries
 * using `get_bspan_key`. It returns:
 * - A negative value if a < b
 * - Zero if a == b
 * - A positive value if a > b
 *
 * This comparison is used to maintain the order of bspan entries in the B-tree.
 *
 * @param a Pointer to the first bspan entry.
 * @param b Pointer to the second bspan entry.
 * @return int Comparison result: negative, zero, or positive.
 */
int compare_bspan(const void *a, const void *b) {
  return (get_bspan_key(a) > get_bspan_key(b)) -
         (get_bspan_key(a) < get_bspan_key(b));
}

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
btree_handle_t *btree_create_bspan_handle() {
  return btree_create_handle(BTREE_TYPE_BSPAN, sizeof(bspan_t), compare_bspan,
                             get_bspan_key);
}

/**
 * @brief Computes the djb2 hash of a given filename.
 *
 * This function implements the classic djb2 hashing algorithm created by
 * Daniel J. Bernstein. It is fast and widely used for hashing strings.
 *
 * @param name The null-terminated string (filename) to be hashed.
 * @return A 32-bit unsigned integer hash value.
 */
uint32_t hash_filename(const char *name) {
  uint32_t hash = 5381;
  int c;
  while ((c = *name++)) {
    hash = ((hash << 5) + hash) + c;
  }
  return hash;
}

/**
 * @brief Retrieves the key (hash of the filename) for a directory entry.
 *
 * This function is used by the B-tree infrastructure to extract the
 * comparison key from a directory entry, which is the result of hashing
 * its filename with `hash_filename`.
 *
 * @param entry Pointer to the `dir_entry_t` structure.
 * @return The hash of the filename as a 32-bit unsigned integer.
 */
uint32_t get_dir_entry_key(const void *entry) {
  return hash_filename(((const dir_entry_t *)entry)->filename);
}

/**
 * @brief Compares two directory entries by filename hash.
 *
 * This function compares two `dir_entry_t` entries based on the hash of their
 * filenames using the djb2 algorithm. If the hashes are equal, it falls back
 * to a full `strcmp` to resolve hash collisions.
 *
 * @param a Pointer to the first `dir_entry_t`.
 * @param b Pointer to the second `dir_entry_t`.
 * @return -1 if a < b, 1 if a > b, 0 if equal.
 */
int compare_dir_entry(const void *a, const void *b) {
  uint32_t ha = get_dir_entry_key(a);
  uint32_t hb = get_dir_entry_key(b);

  if (ha < hb)
    return -1;
  if (ha > hb)
    return 1;

  // In case of hash collision, fallback to full string comparison
  const dir_entry_t *ea = (const dir_entry_t *)a;
  const dir_entry_t *eb = (const dir_entry_t *)b;
  return strcmp(ea->filename, eb->filename);
}

/**
 * @brief Creates a B-tree handle for managing directory entries.
 *
 * This function initializes a new `btree_handle_t` configured to operate
 * on directory entries (`dir_entry_t`) using their hashed filenames as keys.
 *
 * @return A pointer to a newly allocated B-tree handle configured for
 *         directory entries.
 */
btree_handle_t *btree_create_dir_entry_handle() {
  return btree_create_handle(BTREE_TYPE_DIR_ENTRY, sizeof(dir_entry_t),
                             compare_dir_entry, get_dir_entry_key);
}

/**
 * @brief Extracts the key from a recovery entry.
 *
 * This function casts the input pointer to a `rec_entry_t` and returns
 * the `index` field, which serves as the unique identifier or key for
 * the recovery entry in the B-tree.
 *
 * @param entry Pointer to a recovery entry (expected to be of type
 * `rec_entry_t`).
 * @return uint32_t The index value used as the key in the B-tree.
 */
uint32_t get_rec_entry_key(const void *entry) {
  return ((const rec_entry_t *)entry)->recovery_id;
}

/**
 * @brief Compares two recovery entries by their index field.
 *
 * This function compares the keys extracted from two `rec_entry_t` entries
 * using `get_rec_entry_key`. It returns:
 * - A negative value if a < b
 * - Zero if a == b
 * - A positive value if a > b
 *
 * This is used to maintain the sorted order of recovery entries in the B-tree.
 *
 * @param a Pointer to the first recovery entry.
 * @param b Pointer to the second recovery entry.
 * @return int Comparison result: negative, zero, or positive.
 */
int compare_rec_entry(const void *a, const void *b) {
  return (get_rec_entry_key(a) > get_rec_entry_key(b)) -
         (get_rec_entry_key(a) < get_rec_entry_key(b));
}

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
btree_handle_t *btree_create_rec_entry_handle() {
  return btree_create_handle(BTREE_TYPE_RECOVERY, sizeof(rec_entry_t),
                             compare_rec_entry, get_rec_entry_key);
}

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
                                    compare_fn cmp, key_fn get_key) {
  btree_handle_t *handle = malloc(sizeof(btree_handle_t));
  handle->type = type;
  handle->entry_size = entry_size;
  handle->cmp = cmp ? cmp : default_compare;
  handle->get_key = get_key;
  handle->root_block = 0;
  handle->min_degree = compute_min_degree(entry_size);
  return handle;
}

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
                 void *new_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  uint8_t *entries = node->entries;
  uint32_t key = handle->get_key(new_entry);

  uint32_t i = 0;
  while (i < node->num_entries) {
    uint32_t entry_key = handle->get_key(entries + i * handle->entry_size);
    if (key <= entry_key)
      break;
    i++;
  }

  if (i < node->num_entries) {
    uint32_t entry_key = handle->get_key(entries + i * handle->entry_size);
    if (key == entry_key) {
      memcpy(entries + i * handle->entry_size, new_entry, handle->entry_size);
      btree_write_node(disk, handle, node_block, node);
      btree_free_node(node);
      return 1;
    }
  }

  if (node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  int updated = btree_update(disk, handle, node->children[i], new_entry);
  if (updated) {
    btree_write_node(disk, handle, node_block, node);
  }
  btree_free_node(node);
  return updated;
}

/**
 * @brief Writes a B-tree node to disk.
 *
 * Serializes the in-memory B-tree node structure into a fixed-size block
 * and writes it to the specified block number on disk.
 *
 * @param disk Pointer to the open disk file.
 * @param handle Pointer to the B-tree handle containing entry configuration.
 * @param block The block number where the node should be written.
 * @param node Pointer to the in-memory B-tree node structure.
 * @return 0 on success, -1 on failure.
 */
int btree_write_node(FILE *disk, btree_handle_t *handle, uint32_t block,
                     btree_node_t *node) {
  uint8_t buffer[BLOCK_SIZE] = {0};
  size_t offset = 0;

  memcpy(buffer + offset, &node->is_leaf, sizeof(uint8_t));
  offset += sizeof(uint8_t);

  memcpy(buffer + offset, &node->num_entries, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  memcpy(buffer + offset, node->entries,
         handle->entry_size * (2 * handle->min_degree - 1));
  offset += handle->entry_size * (2 * handle->min_degree - 1);

  memcpy(buffer + offset, node->children,
         sizeof(uint32_t) * (2 * handle->min_degree));

  if (write_block(disk, block, buffer, sizeof(buffer)) != 0) {
    fprintf(stderr, "Failed to write B-tree node at block %u\n", block);
    return -1;
  }

  return 0;
}

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
                              uint32_t block) {
  uint8_t buffer[BLOCK_SIZE];
  read_block(disk, block, buffer, sizeof(buffer));
  size_t offset = 0;
  btree_node_t *node = malloc(sizeof(btree_node_t));
  memcpy(&node->is_leaf, buffer + offset, sizeof(uint8_t));
  offset += sizeof(uint8_t);
  memcpy(&node->num_entries, buffer + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  node->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
  memcpy(node->entries, buffer + offset,
         handle->entry_size * (2 * handle->min_degree - 1));
  offset += handle->entry_size * (2 * handle->min_degree - 1);
  node->children = calloc(2 * handle->min_degree, sizeof(uint32_t));
  memcpy(node->children, buffer + offset,
         sizeof(uint32_t) * (2 * handle->min_degree));

  return node;
}

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
void btree_free_node(btree_node_t *node) {
  free(node->entries);
  free(node->children);
  free(node);
}

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
                 uint32_t key, void *output) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  uint8_t *entries = node->entries;

  uint32_t i = 0;
  while (i < node->num_entries) {
    uint32_t entry_key = handle->get_key(entries + i * handle->entry_size);
    if (key <= entry_key)
      break;
    i++;
  }

  if (i < node->num_entries) {
    uint32_t entry_key = handle->get_key(entries + i * handle->entry_size);
    if (key == entry_key) {
      memcpy(output, entries + i * handle->entry_size, handle->entry_size);
      btree_free_node(node);
      return 1;
    }
  }

  if (node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  uint32_t child_block = node->children[i];
  btree_free_node(node);
  return btree_search(disk, handle, child_block, key, output);
}

/**
 * @brief Searches for an entry in the B-tree using the handle's compare
 *        function (full entry comparison), not just the numeric key.
 *
 * This is useful for data types whose ordering depends on a composite key
 * (e.g., directory entries ordered by (hash(filename), filename)).
 *
 * @param disk Pointer to the open disk file.
 * @param handle Pointer to the B-tree handle containing type/logic functions.
 * @param node_block Block number of the node to begin searching from.
 * @param needle Pointer to an in-memory entry (same type as tree entries)
 *               used for comparison and navigation.
 * @param output Pointer where the found entry will be copied if present.
 * @return int 1 if found, 0 otherwise.
 */
int btree_search_entry(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                       const void *needle, void *output) {
  if (node_block == 0)
    return 0;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  uint8_t *entries = node->entries;
  int i = 0;
  while (i < (int)node->num_entries) {
    void *e = entries + i * handle->entry_size;
    int cmp = handle->cmp(e, needle);
    (void)node_block; // suppress debug-only variables
    if (cmp >= 0)
      break;
    i++;
  }

  if (i < (int)node->num_entries) {
    void *e = entries + i * handle->entry_size;
    int cmp_eq = handle->cmp(e, needle);
    if (cmp_eq == 0) {
      memcpy(output, e, handle->entry_size);
      btree_free_node(node);
      return 1;
    }
    // If key > entries[i], descend to right child i+1
    if (!node->is_leaf && cmp_eq < 0) {
      uint32_t right_child = node->children[i + 1];
      btree_free_node(node);
      return btree_search_entry(disk, handle, right_child, needle, output);
    }
  }

  if (node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  // Otherwise, key is less than entries[i] (or i == num_entries): go to child i
  uint32_t child_block = node->children[(uint32_t)i];
  (void)i;
  (void)child_block;
  btree_free_node(node);
  return btree_search_entry(disk, handle, child_block, needle, output);
}

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
int btree_insert(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                 void *entry) {
  if (handle->root_block == 0) {
    btree_node_t *root = malloc(sizeof(btree_node_t));
    if (!root)
      return -1;
    root->is_leaf = 1;
    root->num_entries = 0;
    root->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
    root->children = calloc(2 * handle->min_degree, sizeof(uint32_t));
    if (!root->entries || !root->children) {
      btree_free_node(root);
      return -1;
    }

    handle->root_block = allocate_single_block(disk, sb);
    if (handle->root_block == INVALID_BLOCK) {
      btree_free_node(root);
      return -1;
    }

    if (btree_insert_nonfull(disk, sb, handle, handle->root_block, root,
                             entry) != 0 ||
        btree_write_node(disk, handle, handle->root_block, root) != 0) {
      btree_free_node(root);
      return -1;
    }

    btree_free_node(root);
    return 0;
  }

  btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
  if (!root)
    return -1;

  if (root->num_entries == (uint32_t)(2 * handle->min_degree - 1)) {
    btree_node_t *new_root = malloc(sizeof(btree_node_t));
    if (!new_root) {
      btree_free_node(root);
      return -1;
    }
    new_root->is_leaf = 0;
    new_root->num_entries = 0;
    new_root->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
    new_root->children = calloc(2 * handle->min_degree, sizeof(uint32_t));
    if (!new_root->entries || !new_root->children) {
      btree_free_node(new_root);
      btree_free_node(root);
      return -1;
    }

    uint32_t new_root_block = allocate_single_block(disk, sb);
    if (new_root_block == INVALID_BLOCK) {
      btree_free_node(new_root);
      btree_free_node(root);
      return -1;
    }

    new_root->children[0] = handle->root_block;

    if (btree_split_child(disk, sb, handle, new_root_block, new_root, 0) != 0 ||
        btree_insert_nonfull(disk, sb, handle, new_root_block, new_root,
                             entry) != 0 ||
        btree_write_node(disk, handle, new_root_block, new_root) != 0) {
      btree_free_node(new_root);
      btree_free_node(root);
      return -1;
    }

    handle->root_block = new_root_block;
    btree_free_node(new_root);
  } else {
    if (btree_insert_nonfull(disk, sb, handle, handle->root_block, root,
                             entry) != 0 ||
        btree_write_node(disk, handle, handle->root_block, root) != 0) {
      btree_free_node(root);
      return -1;
    }
  }

  btree_free_node(root);
  return 0;
}

/**
 * @brief Inserts an entry into a non-full B-tree node.
 *
 * This function is used internally during the insertion process when the
 * current node has room for at least one more entry (i.e., it is not full). If
 * the node is a leaf, the entry is inserted at the correct sorted position. If
 * the node is internal, the function determines the correct child to descend
 * into, and recursively inserts the entry. If the child is full, it is split
 * before descending.
 *
 * This function ensures that insertions always happen in non-full nodes,
 * preserving the B-tree invariants.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock (used for block allocation if needed).
 * @param handle Pointer to the B-tree handle (contains entry size and key
 * functions).
 * @param node Pointer to the current node in memory (assumed to be non-full).
 * @param entry Pointer to the entry to be inserted.
 */
static int btree_insert_nonfull(FILE *disk, superblock_t *sb,
                                btree_handle_t *handle, uint32_t node_block,
                                btree_node_t *node, void *entry) {
  int i = node->num_entries - 1;
  uint8_t *entries = node->entries;
  uint32_t key = handle->get_key(entry);

  if (node->is_leaf) {
    while (i >= 0) {
      void *e = entries + i * handle->entry_size;
      int cmp = handle->cmp ? handle->cmp(e, entry)
                            : ((int)handle->get_key(e) - (int)key);
      if (cmp <= 0)
        break;
      memcpy(entries + (i + 1) * handle->entry_size,
             entries + i * handle->entry_size, handle->entry_size);
      i--;
    }
    memcpy(entries + (i + 1) * handle->entry_size, entry, handle->entry_size);
    node->num_entries++;
    // persist leaf update immediately
    return btree_write_node(disk, handle, node_block, node);
  } else {
    while (i >= 0) {
      void *e = entries + i * handle->entry_size;
      int cmp = handle->cmp ? handle->cmp(e, entry)
                            : ((int)handle->get_key(e) - (int)key);
      if (cmp <= 0)
        break;
      i--;
    }
    i++;
    uint32_t child_block = node->children[i];
    btree_node_t *child = btree_read_node(disk, handle, child_block);
    if (!child)
      return -1;

    if (child->num_entries == (uint32_t)(2 * handle->min_degree - 1)) {
      if (btree_split_child(disk, sb, handle, node_block, node, i) != 0) {
        btree_free_node(child);
        return -1;
      }
      {
        void *mid = entries + i * handle->entry_size;
        int cmp = handle->cmp ? handle->cmp(entry, mid)
                              : ((int)key - (int)handle->get_key(mid));
        if (cmp > 0)
          i++;
      }
      btree_free_node(child);
      child_block = node->children[i];
      child = btree_read_node(disk, handle, child_block);
      if (!child)
        return -1;
    }

    int result =
        btree_insert_nonfull(disk, sb, handle, child_block, child, entry);
    if (result == 0) {
      // persist child and updated parent
      if (btree_write_node(disk, handle, child_block, child) != 0)
        result = -1;
      if (btree_write_node(disk, handle, node_block, node) != 0)
        result = -1;
    }

    btree_free_node(child);
    return result;
  }
}

/**
 * @brief Splits a full child node in a B-tree.
 *
 * This function is called when a child node at position `index` in the `parent`
 * node is full and cannot accommodate a new entry. It splits the child node
 * into two separate nodes:
 * - The left half remains in the original child node.
 * - The right half is moved to a newly allocated node.
 * - The middle key is promoted to the parent node.
 *
 * After the split, the parent node has one more key and one more child pointer,
 * and the B-tree invariants are preserved. This operation is crucial for
 * keeping insertions efficient and balanced in a B-tree.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock (used for allocating a new block for the
 * new node).
 * @param handle Pointer to the B-tree handle with configuration and entry
 * logic.
 * @param parent Pointer to the parent node in memory where the split key will
 * be inserted.
 * @param index Index of the child in the parent’s `children` array that needs
 * to be split.
 */
static int btree_split_child(FILE *disk, superblock_t *sb,
                             btree_handle_t *handle, uint32_t parent_block,
                             btree_node_t *parent, int index) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[index]);
  if (!child)
    return -1;

  btree_node_t *new_node = malloc(sizeof(btree_node_t));
  if (!new_node) {
    btree_free_node(child);
    return -1;
  }
  new_node->is_leaf = child->is_leaf;
  new_node->num_entries = handle->min_degree - 1;
  new_node->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
  new_node->children = calloc(2 * handle->min_degree, sizeof(uint32_t));
  if (!new_node->entries || !new_node->children) {
    btree_free_node(child);
    btree_free_node(new_node);
    return -1;
  }

  uint32_t new_block = allocate_single_block(disk, sb);
  if (new_block == INVALID_BLOCK) {
    btree_free_node(child);
    btree_free_node(new_node);
    return -1;
  }

  memcpy(new_node->entries,
         (char *)child->entries + handle->min_degree * handle->entry_size,
         (handle->min_degree - 1) * handle->entry_size);

  if (!child->is_leaf) {
    memcpy(new_node->children, child->children + handle->min_degree,
           handle->min_degree * sizeof(uint32_t));
  }

  child->num_entries = handle->min_degree - 1;

  for (int j = (int)parent->num_entries; j > index; j--) {
    parent->children[j + 1] = parent->children[j];
  }
  parent->children[index + 1] = new_block;

  for (int j = (int)parent->num_entries - 1; j >= index; j--) {
    memcpy((char *)parent->entries + (j + 1) * handle->entry_size,
           (char *)parent->entries + j * handle->entry_size,
           handle->entry_size);
  }

  memcpy((char *)parent->entries + index * handle->entry_size,
         (char *)child->entries + (handle->min_degree - 1) * handle->entry_size,
         handle->entry_size);

  parent->num_entries++;

  int ok = 0;
  ok |= btree_write_node(disk, handle, parent->children[index], child);
  ok |= btree_write_node(disk, handle, new_block, new_node);
  ok |= btree_write_node(disk, handle, parent_block, parent);

  btree_free_node(child);
  btree_free_node(new_node);
  return (ok == 0) ? 0 : -1;
}

/**
 * @brief Removes an entry from the B-tree based on its key.
 *
 * This is the public interface for removing an entry from a B-tree. It
 * initiates a recursive deletion process starting from the tree root.
 *
 * Internally, the actual logic is handled by `btree_remove_internal`, which
 * updates the tree structure, balances nodes if necessary, and writes changes
 * to disk.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock, used for block deallocation if
 * necessary.
 * @param handle Pointer to the B-tree handle structure.
 * @param key The key of the entry to remove from the tree.
 * @return int 1 if the key was found and removed, 0 if the key was not found.
 */
int btree_remove(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                 uint32_t key) {
  int success = 0;
  uint32_t old_root = handle ? handle->root_block : 0;
  btree_remove_internal(disk, sb, handle, handle->root_block, key, &success);

  if (success && handle && handle->root_block != 0) {
    btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
    if (root) {
      if (root->num_entries == 0) {
        if (root->is_leaf) {
          uint32_t empty_block = handle->root_block;
          handle->root_block = 0;
          (void)deallocate_single_block(disk, sb, empty_block);
        } else {
          uint32_t new_root_block = root->children[0];
          uint32_t empty_block = handle->root_block;
          handle->root_block = new_root_block;
          (void)deallocate_single_block(disk, sb, empty_block);
        }
      }
      btree_free_node(root);
    }
  } else if (success && handle && handle->root_block == 0 && old_root != 0) {
    (void)deallocate_single_block(disk, sb, old_root);
  }

  return success;
}

/**
 * @brief Internal recursive function to remove a key from a B-tree node.
 *
 * This function handles all cases of B-tree deletion, including:
 * - Removing a key from a leaf node.
 * - Removing a key from an internal node by replacing it with its predecessor
 * or successor.
 * - Merging child nodes when neither sibling has enough entries to lend one.
 * - Borrowing an entry from a sibling when possible to restore minimum
 * occupancy.
 *
 * It also ensures the B-tree invariants are preserved during removal and
 * interacts with the `btree_merge()` and `btree_fill()` routines which may
 * trigger block deallocation when a node becomes empty after a merge.
 *
 * @param disk Pointer to the open FILE representing the disk image.
 * @param sb Pointer to the superblock, used for block deallocation if
 * necessary.
 * @param handle Pointer to the B-tree handle that contains tree metadata.
 * @param node_block Block number of the node to inspect and modify.
 * @param key The key to be removed from the B-tree.
 * @param success Output parameter set to 1 if the key was found and removed, or
 * 0 if not found.
 */
static void btree_remove_internal(FILE *disk, superblock_t *sb,
                                  btree_handle_t *handle, uint32_t node_block,
                                  uint32_t key, int *success) {
  if (node_block == 0) {
    *success = 0;
    return;
  }
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  uint32_t i = 0;
  while (i < node->num_entries) {
    uint32_t ekey =
        handle->get_key((uint8_t *)node->entries + i * handle->entry_size);
    if (key <= ekey)
      break;
    i++;
  }

  if (i < node->num_entries) {
    uint32_t ekey =
        handle->get_key((uint8_t *)node->entries + i * handle->entry_size);
    if (key == ekey) {
      if (node->is_leaf) {
        btree_shift_left(node->entries, handle->entry_size, i,
                         node->num_entries);
        node->num_entries--;
        btree_write_node(disk, handle, node_block, node);
        *success = 1;
      } else {
        btree_node_t *pred = btree_read_node(disk, handle, node->children[i]);
        if (pred->num_entries >= (uint32_t)handle->min_degree) {
          memcpy((uint8_t *)node->entries + i * handle->entry_size,
                 (uint8_t *)pred->entries +
                     (pred->num_entries - 1) * handle->entry_size,
                 handle->entry_size);
          btree_write_node(disk, handle, node_block, node);
          uint32_t pred_key =
              handle->get_key((uint8_t *)pred->entries +
                              (pred->num_entries - 1) * handle->entry_size);
          btree_free_node(pred);
          btree_remove_internal(disk, sb, handle, node->children[i], pred_key,
                                success);
        } else {
          btree_node_t *succ =
              btree_read_node(disk, handle, node->children[i + 1]);
          if (succ->num_entries >= (uint32_t)handle->min_degree) {
            memcpy((uint8_t *)node->entries + i * handle->entry_size,
                   (uint8_t *)succ->entries, handle->entry_size);
            btree_write_node(disk, handle, node_block, node);
            uint32_t succ_key = handle->get_key(succ->entries);
            btree_free_node(succ);
            btree_remove_internal(disk, sb, handle, node->children[i + 1],
                                  succ_key, success);
          } else {
            btree_merge(disk, sb, handle, node_block, node, i);
            btree_remove_internal(disk, sb, handle, node->children[i], key,
                                  success);
          }
          // succ was freed in the >= branch and merged into left in the else
          // branch; do not double-free here.
        }
      }
    }
  } else {
    if (node->is_leaf) {
      *success = 0;
    } else {
      int child_idx = i;
      btree_node_t *child =
          btree_read_node(disk, handle, node->children[child_idx]);
      if (child->num_entries < (uint32_t)handle->min_degree) {
        btree_fill(disk, sb, handle, node_block, node, child_idx);
        btree_write_node(disk, handle, node_block, node);
        if (child_idx > (int)node->num_entries)
          child_idx = node->num_entries;
      }
      btree_free_node(child);
      btree_remove_internal(disk, sb, handle, node->children[child_idx], key,
                            success);
    }
  }

  btree_free_node(node);
}

/** Entry-based remove (uses comparator for exact-match ordering) */
/* Removed entry-based remove (unstable); keeping key-based API only. */
/**
 * @brief Retrieve the rightmost (maximum) entry in a subtree.
 *
 * Descends to the right-most child of @p node_block and copies the final
 * entry into @p out_entry. This utility is used by the delete algorithm when
 * replacing a key with its predecessor during merges/borrows.
 *
 * @param disk       Open disk file backing the tree.
 * @param handle     B-tree handle describing node layout.
 * @param node_block Subtree root to inspect.
 * @param out_entry  Destination buffer for the maximum entry.
 * @return 1 if a value was copied; 0 if the subtree is empty or unreadable.
 */
static int btree_find_max_entry(FILE *disk, btree_handle_t *handle,
                                uint32_t node_block, void *out_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;
  int ok;
  if (node->is_leaf) {
    if (node->num_entries > 0) {
      memcpy(out_entry,
             (uint8_t *)node->entries +
                 (node->num_entries - 1) * handle->entry_size,
             handle->entry_size);
      ok = 1;
    } else {
      ok = 0;
    }
  } else {
    ok = btree_find_max_entry(disk, handle, node->children[node->num_entries],
                              out_entry);
  }
  btree_free_node(node);
  return ok;
}

/**
 * @brief Comparator-driven removal helper.
 *
 * Recursively locates and deletes @p needle from the subtree rooted at
 * @p node_block, using the handle's comparator instead of key lookups. The
 * routine mirrors the standard B-tree delete algorithm: for leaf nodes it
 * shifts entries in-place; for internal nodes it borrows predecessors or
 * successors, and performs merges/borrows to preserve the minimum degree.
 *
 * @param disk       Open disk file backing the tree.
 * @param sb         Loaded superblock (needed for block deallocation).
 * @param handle     B-tree handle that owns comparator/key hooks.
 * @param node_block Current node to inspect.
 * @param needle     Entry that must match via `handle->cmp`.
 * @param success    Output flag set to 1 when the entry is removed.
 */
static void btree_remove_entry_internal(FILE *disk, superblock_t *sb,
                                        btree_handle_t *handle,
                                        uint32_t node_block, const void *needle,
                                        int *success) {
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node) {
    *success = 0;
    return;
  }

  uint32_t i = 0;
  while (i < node->num_entries) {
    void *e = (uint8_t *)node->entries + i * handle->entry_size;
    int cmp = handle->cmp(e, needle);
    if (cmp >= 0)
      break;
    i++;
  }

  int found_here = 0;
  if (i < node->num_entries) {
    void *e = (uint8_t *)node->entries + i * handle->entry_size;
    found_here = (handle->cmp(e, needle) == 0);
  }

  if (found_here) {
    // Case: found in this node
    if (node->is_leaf) {
      // Remove directly
      btree_shift_left(node->entries, handle->entry_size, (int)i,
                       (int)node->num_entries);
      node->num_entries--;
      (void)btree_write_node(disk, handle, node_block, node);
      *success = 1;
      btree_free_node(node);
      return;
    }
    // Internal node: need predecessor or successor
    uint32_t left_block = node->children[i];
    uint32_t right_block = node->children[i + 1];
    btree_node_t *left = btree_read_node(disk, handle, left_block);
    btree_node_t *right = btree_read_node(disk, handle, right_block);
    if (left && left->num_entries >= (uint32_t)handle->min_degree) {
      // Replace with predecessor
      uint8_t pred[handle->entry_size];
      btree_free_node(right);
      int ok = btree_find_max_entry(disk, handle, left_block, pred);
      if (ok) {
        memcpy((uint8_t *)node->entries + i * handle->entry_size, pred,
               handle->entry_size);
        (void)btree_write_node(disk, handle, node_block, node);
        btree_free_node(left);
        btree_remove_entry_internal(disk, sb, handle, left_block, pred,
                                    success);
        btree_free_node(node);
        return;
      }
    } else if (right && right->num_entries >= (uint32_t)handle->min_degree) {
      // Replace with successor
      uint8_t succ[handle->entry_size];
      btree_free_node(left);
      // Find successor = minimum entry in right subtree
      // Local generic implementation
      int ok = 0;
      {
        uint32_t nb = right_block;
        while (nb) {
          btree_node_t *n = btree_read_node(disk, handle, nb);
          if (!n)
            break;
          if (n->is_leaf) {
            if (n->num_entries > 0) {
              memcpy(succ, n->entries, handle->entry_size);
              ok = 1;
            }
            btree_free_node(n);
            break;
          }
          uint32_t next = n->children[0];
          btree_free_node(n);
          nb = next;
        }
      }
      if (ok) {
        memcpy((uint8_t *)node->entries + i * handle->entry_size, succ,
               handle->entry_size);
        (void)btree_write_node(disk, handle, node_block, node);
        btree_free_node(right);
        btree_remove_entry_internal(disk, sb, handle, right_block, succ,
                                    success);
        btree_free_node(node);
        return;
      }
    } else {
      // Merge left + key + right, then recurse into merged
      btree_merge(disk, sb, handle, node_block, node, (int)i);
      // After merge, the merged child stays at children[i]
      uint32_t merged_block = node->children[i];
      (void)btree_write_node(disk, handle, node_block, node);
      btree_remove_entry_internal(disk, sb, handle, merged_block, needle,
                                  success);
      btree_free_node(node);
      return;
    }
    // Defensive free
    if (left)
      btree_free_node(left);
    if (right)
      btree_free_node(right);
    btree_free_node(node);
    *success = 0;
    return;
  }

  // Not found in this node
  if (node->is_leaf) {
    btree_free_node(node);
    *success = 0;
    return;
  }

  // Choose child to descend
  int child_idx = (int)i;
  uint32_t left_sib_block = (child_idx > 0) ? node->children[child_idx - 1] : 0;
  uint32_t child_block = node->children[child_idx];
  uint32_t right_sib_block =
      (child_idx < (int)node->num_entries) ? node->children[child_idx + 1] : 0;

  btree_node_t *child = btree_read_node(disk, handle, child_block);
  if (!child) {
    btree_free_node(node);
    *success = 0;
    return;
  }
  if (child->num_entries < (uint32_t)handle->min_degree) {
    // Try to borrow, else merge appropriately
    if (left_sib_block) {
      btree_node_t *left = btree_read_node(disk, handle, left_sib_block);
      if (left && left->num_entries >= (uint32_t)handle->min_degree) {
        btree_borrow_from_prev(disk, handle, node_block, node, child_idx);
      } else {
        // Merge with left sibling
        btree_merge(disk, sb, handle, node_block, node, child_idx - 1);
        child_idx -= 1;
        child_block = node->children[child_idx];
      }
      if (left)
        btree_free_node(left);
    } else if (right_sib_block) {
      btree_node_t *right = btree_read_node(disk, handle, right_sib_block);
      if (right && right->num_entries >= (uint32_t)handle->min_degree) {
        btree_borrow_from_next(disk, handle, node_block, node, child_idx);
      } else {
        // Merge with right sibling at child_idx
        btree_merge(disk, sb, handle, node_block, node, child_idx);
        // child_idx remains the same; merged block at children[child_idx]
        child_block = node->children[child_idx];
      }
      if (right)
        btree_free_node(right);
    }
    (void)btree_write_node(disk, handle, node_block, node);
  }
  if (child_idx > (int)node->num_entries)
    child_idx = node->num_entries;
  btree_free_node(child);

  // Recurse
  child_block = node->children[child_idx];
  if (child_block == 0) {
    btree_free_node(node);
    *success = 0;
    return;
  }
  btree_remove_entry_internal(disk, sb, handle, child_block, needle, success);
  btree_free_node(node);
}

/**
 * @brief Remove an entry using the handle's comparator (exact-match).
 *
 * Suitable for trees where ordering is defined by a comparator (e.g.,
 * directory entries ordered by hash+name). Returns 1 if the entry was
 * found and removed, 0 if not found, -1 on error.
 */
int btree_remove_entry(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                       const void *entry) {
  if (!handle || handle->root_block == 0)
    return 0;
  int success = 0;
  btree_remove_entry_internal(disk, sb, handle, handle->root_block, entry,
                              &success);
  // Shrink root if empty
  if (handle->root_block != 0) {
    btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
    if (root) {
      if (root->num_entries == 0) {
        if (root->is_leaf) {
          uint32_t old_root = handle->root_block;
          handle->root_block = 0;
          (void)deallocate_single_block(disk, sb, old_root);
        } else {
          uint32_t new_root_block = root->children[0];
          uint32_t old_root = handle->root_block;
          handle->root_block = new_root_block;
          (void)deallocate_single_block(disk, sb, old_root);
        }
      }
      btree_free_node(root);
    }
  }
  return success;
}

/**
 * @brief Merges two child nodes of a parent in a B-tree.
 *
 * This function merges the child at position `idx` and its immediate right
 * sibling (at `idx + 1`) into a single node. The entry from the parent node
 * that separates the two children is moved down into the new merged node.
 *
 * This is used during deletion when both child nodes have fewer than the
 * minimum number of entries and neither can lend an entry. The merge reduces
 * the height of the tree when applied at the root and maintains balance
 * otherwise.
 *
 * After the merge:
 * - The parent has one fewer entry and child.
 * - The left child now contains all entries and children from the right child.
 * - The right child's block is deallocated.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock (for bitmap updates).
 * @param handle Pointer to the B-tree handle.
 * @param parent Pointer to the parent node containing the children to be
 * merged.
 * @param idx Index of the left child to merge (merges child[idx] and
 * child[idx+1]).
 */
static void btree_merge(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                        uint32_t parent_block, btree_node_t *parent, int idx) {
  uint32_t left_block = parent->children[idx];
  uint32_t right_block = parent->children[idx + 1];

  btree_node_t *left = btree_read_node(disk, handle, left_block);
  btree_node_t *right = btree_read_node(disk, handle, right_block);

  uint32_t left_entries = left->num_entries;

  memcpy((uint8_t *)left->entries + left_entries * handle->entry_size,
         (uint8_t *)parent->entries + idx * handle->entry_size,
         handle->entry_size);
  left_entries++;

  memcpy((uint8_t *)left->entries + left_entries * handle->entry_size,
         (uint8_t *)right->entries, handle->entry_size * right->num_entries);

  if (!left->is_leaf) {
    memcpy(left->children + left_entries, right->children,
           sizeof(uint32_t) * (right->num_entries + 1));
  }

  left->num_entries = left_entries + right->num_entries;

  for (uint32_t i = (uint32_t)(idx + 1); i < parent->num_entries; i++) {
    memcpy((uint8_t *)parent->entries + (i - 1) * handle->entry_size,
           (uint8_t *)parent->entries + i * handle->entry_size,
           handle->entry_size);
    parent->children[i] = parent->children[i + 1];
  }

  parent->num_entries--;
  parent->children[parent->num_entries + 1] = 0;

  btree_write_node(disk, handle, left_block, left);
  btree_write_node(disk, handle, parent_block, parent);

  deallocate_single_block(disk, sb, right_block);

  btree_free_node(left);
  btree_free_node(right);
}

/**
 * @brief Ensures that the child node at a given index has at least the minimum
 * number of entries.
 *
 * This function is called during deletion when the child node at position `idx`
 * has fewer than the minimum required number of entries. It attempts to
 * rebalance the tree by:
 * - Borrowing an entry from the left sibling, if possible.
 * - Borrowing an entry from the right sibling, if possible.
 * - Merging with a sibling node if neither sibling can spare an entry.
 *
 * This ensures that recursive deletion operations can proceed without violating
 * B-tree balance invariants.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock, used for block deallocation if
 * necessary.
 * @param handle Pointer to the B-tree handle.
 * @param parent Pointer to the parent node that contains the underfull child.
 * @param idx Index of the underfull child in the parent's `children` array.
 */
static void btree_fill(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                       uint32_t parent_block, btree_node_t *parent, int idx) {
  if (idx > 0) {
    btree_node_t *left =
        btree_read_node(disk, handle, parent->children[idx - 1]);
    if (left->num_entries >= (uint32_t)handle->min_degree) {
      btree_borrow_from_prev(disk, handle, parent_block, parent, idx);
      btree_free_node(left);
      return;
    }
    btree_free_node(left);
  }
  if ((uint32_t)idx < parent->num_entries) {
    btree_node_t *right =
        btree_read_node(disk, handle, parent->children[idx + 1]);
    if (right->num_entries >= (uint32_t)handle->min_degree) {
      btree_borrow_from_next(disk, handle, parent_block, parent, idx);
      btree_free_node(right);
      return;
    }
    btree_free_node(right);
  }
  btree_merge(disk, sb, handle, parent_block, parent,
              (idx > 0) ? idx - 1 : idx);
}

/**
 * @brief Borrows an entry from the left sibling of a B-tree node.
 *
 * This function is used during deletion when a child node has fewer than the
 * minimum number of entries (`t - 1`). If its immediate left sibling has at
 * least `t` entries, one entry is borrowed to balance the tree.
 *
 * The borrowing process:
 * - The last entry of the left sibling is moved up to the parent.
 * - The parent entry above the underfull child is moved down into the child.
 * - If internal nodes are involved, the corresponding child pointer is also
 * transferred.
 *
 * This preserves the B-tree properties without requiring a merge.
 *
 * @param disk Pointer to the open disk file.
 * @param handle Pointer to the B-tree handle.
 * @param parent Pointer to the parent node containing both the underfull child
 * and its left sibling.
 * @param idx Index of the underfull child in the parent's `children` array.
 */
static void btree_borrow_from_prev(FILE *disk, btree_handle_t *handle,
                                   uint32_t parent_block, btree_node_t *parent,
                                   int idx) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *sibling =
      btree_read_node(disk, handle, parent->children[idx - 1]);

  for (int i = child->num_entries - 1; i >= 0; i--) {
    memcpy((uint8_t *)child->entries + (i + 1) * handle->entry_size,
           (uint8_t *)child->entries + i * handle->entry_size,
           handle->entry_size);
  }

  if (!child->is_leaf) {
    for (int i = child->num_entries; i >= 0; i--) {
      child->children[i + 1] = child->children[i];
    }
  }

  memcpy(child->entries,
         (uint8_t *)parent->entries + (idx - 1) * handle->entry_size,
         handle->entry_size);

  if (!child->is_leaf) {
    child->children[0] = sibling->children[sibling->num_entries];
  }

  memcpy((uint8_t *)parent->entries + (idx - 1) * handle->entry_size,
         (uint8_t *)sibling->entries +
             (sibling->num_entries - 1) * handle->entry_size,
         handle->entry_size);

  child->num_entries++;
  sibling->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], child);
  btree_write_node(disk, handle, parent->children[idx - 1], sibling);
  btree_write_node(disk, handle, parent_block, parent);

  btree_free_node(child);
  btree_free_node(sibling);
}

/**
 * @brief Borrows an entry from the right sibling of a B-tree node.
 *
 * This function is used during deletion when a child node has fewer than the
 * minimum number of entries (`t - 1`). If its immediate right sibling has at
 * least `t` entries, one entry is borrowed to balance the tree.
 *
 * The borrowing process:
 * - The first entry of the right sibling is moved up to the parent.
 * - The parent entry above the underfull child is moved down into the child.
 * - If internal nodes are involved, the corresponding child pointer is also
 * transferred.
 *
 * This operation ensures continued adherence to B-tree balance properties
 * without requiring a node merge.
 *
 * @param disk Pointer to the open disk file.
 * @param handle Pointer to the B-tree handle.
 * @param parent Pointer to the parent node containing both the underfull child
 * and its right sibling.
 * @param idx Index of the underfull child in the parent's `children` array.
 */
static void btree_borrow_from_next(FILE *disk, btree_handle_t *handle,
                                   uint32_t parent_block, btree_node_t *parent,
                                   int idx) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *sibling =
      btree_read_node(disk, handle, parent->children[idx + 1]);

  memcpy((uint8_t *)child->entries + child->num_entries * handle->entry_size,
         (uint8_t *)parent->entries + idx * handle->entry_size,
         handle->entry_size);

  if (!child->is_leaf) {
    child->children[child->num_entries + 1] = sibling->children[0];
  }

  memcpy((uint8_t *)parent->entries + idx * handle->entry_size,
         sibling->entries, handle->entry_size);

  for (uint32_t i = 1; i < sibling->num_entries; i++) {
    memcpy((uint8_t *)sibling->entries + (i - 1) * handle->entry_size,
           (uint8_t *)sibling->entries + i * handle->entry_size,
           handle->entry_size);
  }

  if (!sibling->is_leaf) {
    if (sibling->num_entries > 0) {
      uint32_t end = sibling->num_entries - 1;
      for (uint32_t i = 0; i <= end; i++) {
        sibling->children[i] = sibling->children[i + 1];
      }
    }
  }

  child->num_entries++;
  sibling->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], child);
  btree_write_node(disk, handle, parent->children[idx + 1], sibling);
  btree_write_node(disk, handle, parent_block, parent);

  btree_free_node(child);
  btree_free_node(sibling);
}

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
 * @return 0 on success, -1 on failure (e.g., deallocation errors).
 */
int btree_deallocate_all(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                         uint32_t node_block) {
  if (node_block == INVALID_BLOCK || node_block == 0)
    return 0;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node) {
    fprintf(stderr, "Failed to read B-tree node at block %u.\n", node_block);
    return -1;
  }

  // Recursively deallocate children
  if (!node->is_leaf) {
    for (uint32_t i = 0; i <= node->num_entries; i++) {
      if (btree_deallocate_all(disk, sb, handle, node->children[i]) != 0) {
        btree_free_node(node);
        return -1;
      }
    }
  }

  // Deallocate data blocks if this is a BSPAN tree
  if (handle->type == BTREE_TYPE_BSPAN) {
    for (uint32_t i = 0; i < node->num_entries; i++) {
      bspan_t *entry = (bspan_t *)(node->entries + i * handle->entry_size);
      if (entry->length == 0)
        continue;
      if (entry->disk_blk >= sb->total_blocks ||
          entry->disk_blk + entry->length > sb->total_blocks) {
        fprintf(stderr,
                "btree_deallocate_all: skipping invalid span "
                "(disk=%u len=%u total=%u)\n",
                entry->disk_blk, entry->length, sb->total_blocks);
        continue;
      }
      if (deallocate_bspan(disk, sb, entry) != 0) {
        btree_free_node(node);
        return -1;
      }
    }
  }

  // Deallocate the node block itself
  if (deallocate_single_block(disk, sb, node_block) != 0) {
    btree_free_node(node);
    return -1;
  }

  btree_free_node(node);
  return 0;
}
