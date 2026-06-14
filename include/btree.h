/**
 * @file btree.h
 * @brief Public interface for the B-tree implementation in the BSFS filesystem.
 *
 * This header defines the core structures and function prototypes for handling
 * B-tree operations over disk blocks in the BSFS system. B-trees are used to
 * organize entries such as inodes, block spans (bspan), directory entries and
 * recovery entries.
 *
 * The API supports creation, insertion, search, update, and removal of entries
 * in the B-tree stored on disk, and allows configuration for different types of
 * data.
 */
#ifndef BTREE_BSFS_H
#define BTREE_BSFS_H

#include "../bsfs.h"
#include "alloc.h"
#include <linux/fs.h>
#include <linux/types.h>

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
btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size, btree_compare_fn cmp, btree_key_fn get_key);

// TODO: is struct disk_mapped_inode_t correct?
static inline __u32 get_inode_key(const void *entry) {
  return ((struct disk_mapped_inode_t *)entry)->ino;
}

static inline int compare_inode(const void *a, const void *b) {
  return (get_inode_key(a) > get_inode_key(b)) - (get_inode_key(a) < get_inode_key(b));
}

/**
 * @brief Creates a B-tree handle specifically for managing inode entries.
 *
 * This function initializes and returns a pointer to a `btree_handle_t`
 * configured to operate on entries of type `bsfs_inode`. It sets the appropriate:
 * - Data type identifier (`BTREE_TYPE_INODE`)
 * - Entry size (based on `sizeof(bsfs_inode)`)
 * - Key extraction function (`get_inode_key`)
 * - Comparison function (`compare_inode`)
 *
 * The resulting handle can be used for all B-tree operations involving inodes,
 * such as insertion, search, update, and removal.
 *
 * @return Pointer to a newly allocated and configured `btree_handle_t` for
 * inodes.
 */
static inline btree_handle_t *btree_create_inode_handle(void) {
  return btree_create_handle(BTREE_TYPE_INODE, sizeof(struct disk_mapped_inode_t), compare_inode, get_inode_key);
}

static inline __u32 get_bspan_key(const void *entry) {
  return ((const bspan_t *)entry)->file_blk;
}

static inline int compare_bspan(const void *a, const void *b) {
  return (get_bspan_key(a) > get_bspan_key(b)) - (get_bspan_key(a) < get_bspan_key(b));
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
static inline btree_handle_t *btree_create_bspan_handle(void) {
  return btree_create_handle(BTREE_TYPE_BSPAN, sizeof(bspan_t), compare_bspan, get_bspan_key);
}

static inline __u8 *bentries(struct btree_node *node) {
  // up to: (2 * handle->min_degree - 1) * handle->entry_size;
  return node->node->data;
}

static inline __u32 *bchildren(struct btree_node *node, btree_handle_t *handle) {
  // up to: 2 * handle->min_degree

  size_t children_start = (2 * handle->min_degree - 1) * handle->entry_size;
  return (__u32 *)(node->node->data + children_start);
}

static inline __u32 get_dir_entry_key(const void *entry) {
  return hash_filename(((const struct bsfs_dentry *)entry)->filename);
}

// static inline int namecompare(int len, int maxlen, const char *name, const char *buffer) {
//   if (len < maxlen && buffer[len])
//     return 0;
//   return !memcmp(name, buffer, len);
// }

// TODO: use namecompare ??
static inline int compare_dir_entry(const void *a, const void *b) {
  __u32 ha = get_dir_entry_key(a);
  __u32 hb = get_dir_entry_key(b);

  if (ha < hb)
    return -1;
  if (ha > hb)
    return 1;

  // In case of hash collision, fallback to full string comparison
  const struct bsfs_dentry *ea = (const struct bsfs_dentry *)a;
  const struct bsfs_dentry *eb = (const struct bsfs_dentry *)b;
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
static inline btree_handle_t *btree_create_dir_entry_handle(void) {
  return btree_create_handle(BTREE_TYPE_DIR_ENTRY, sizeof(struct bsfs_dentry), compare_dir_entry, get_dir_entry_key);
}

static inline __u32 get_rec_entry_key(const void *entry) {
  return ((const rec_entry_t *)entry)->recovery_id;
}

static inline int compare_rec_entry(const void *a, const void *b) {
  return (get_rec_entry_key(a) > get_rec_entry_key(b)) - (get_rec_entry_key(a) < get_rec_entry_key(b));
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
static inline btree_handle_t *btree_create_rec_entry_handle(void) {
  return btree_create_handle(BTREE_TYPE_RECOVERY, sizeof(rec_entry_t), compare_rec_entry, get_rec_entry_key);
}

void btree_clear_node(struct super_block *sb, struct btree_node *node);

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
int btree_insert(struct super_block *sb, btree_handle_t *handle, void *entry);

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
int btree_search(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, void **output, struct btree_node **out_node);

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
int btree_search_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle, void *output);

int bspan_search_le(struct super_block *sb, btree_handle_t *handle, uint32_t key, bspan_t *out);

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
int btree_update(struct super_block *sb, btree_handle_t *handle, __u32 node_block, void *new_entry);

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
int btree_remove(struct super_block *sb, btree_handle_t *handle, __u32 key);

/**
 * @brief Remove an entry using the handle's comparator (exact-match).
 *
 * Suitable for trees where ordering is defined by a comparator (e.g.,
 * directory entries ordered by hash+name). Returns 1 if the entry was
 * found and removed, 0 if not found, -1 on error.
 */
int btree_remove_entry(struct super_block *sb, btree_handle_t *handle, const void *entry);

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
int btree_write_node(struct super_block *sb, btree_handle_t *handle, __u32 block, struct btree_node_t *node);

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
int btree_read_node(struct super_block *sb, __u32 block, struct btree_node **out_node);

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
void btree_free_node(struct btree_node *node);

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
int btree_deallocate_all(struct super_block *sb, btree_handle_t *handle, __u32 node_block);

#endif // BTREE_BSFS_H
