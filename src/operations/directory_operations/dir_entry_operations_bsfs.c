#include "../../include/dir_entry_operations_bsfs.h"
#include <stdlib.h>
#include <string.h>

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
dir_t *open_dir(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number) {
  inode_t inode;
  if (!read_inode(disk, inode_tree, inode_number, &inode)) {
    fprintf(stderr, "Failed to load directory inode.\n");
    return NULL;
  }

  if (inode.file_type != 1) { // 1 = directory
    fprintf(stderr, "Inode is not a directory.\n");
    return NULL;
  }

  dir_t *dir = malloc(sizeof(dir_t));
  if (!dir) {
    fprintf(stderr, "Failed to allocate memory for dir_t.\n");
    return NULL;
  }

  dir->inode = inode;
  dir->current_index = 0;
  dir->btree = btree_create_dir_entry_handle();
  dir->btree->root_block = inode.btree_root;

  return dir;
}

/**
 * @brief Closes a directory and frees allocated resources.
 *
 * This function deallocates the memory used by the dir_t structure
 * and its associated B-tree handle.
 *
 * @param dir Pointer to the open directory to be closed.
 * @return 0 on success.
 */
int close_dir(dir_t *dir) {
  if (!dir)
    return -1;

  if (dir->btree) {
    free(dir->btree);
  }

  free(dir);
  return 0;
}

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
int add_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                  dir_entry_t *entry) {
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  dir_entry_t existing;
  uint32_t hash = hash_filename(entry->filename);

  if (btree_search(disk, handle, handle->root_block, hash, &existing)) {
    fprintf(stderr, "Directory entry '%s' already exists.\n", entry->filename);
    free(handle);
    return -1;
  }

  if (btree_insert(disk, sb, handle, entry) != 0) {
    free(handle);
    return -1;
  }

  if (dir->inode.btree_root == 0 && handle->root_block != 0) {
    dir->inode.btree_root = handle->root_block;
    btree_handle_t *inode_handle = btree_create_inode_handle();
    inode_handle->root_block = sb->inode_root;
    (void)update_inode(disk, inode_handle, &dir->inode);
    free(inode_handle);
  }

  free(handle);
  return 0;
}

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
int remove_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                     const char *filename) {
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  uint32_t hash = hash_filename(filename);

  dir_entry_t found;
  if (!btree_search(disk, handle, handle->root_block, hash, &found)) {
    fprintf(stderr, "Directory entry '%s' not found.\n", filename);
    free(handle);
    return -1;
  }

  int ok = btree_remove(disk, sb, handle, hash);
  free(handle);
  return ok;
}

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
 * @return 0 on success, -1 if the entry was not found.
 */
int update_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                     dir_entry_t *entry) {
  (void)sb;
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  uint32_t hash = hash_filename(entry->filename);

  dir_entry_t existing;
  if (!btree_search(disk, handle, handle->root_block, hash, &existing)) {
    fprintf(stderr, "Cannot update: entry '%s' not found in directory.\n",
            entry->filename);
    free(handle);
    return -1;
  }

  int ok = btree_update(disk, handle, handle->root_block, entry);
  free(handle);
  return ok;
}

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
                     dir_entry_t *result) {
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  dir_entry_t temp;
  strncpy(temp.filename, name, MAX_FILENAME);
  temp.filename[MAX_FILENAME - 1] = '\0';

  uint32_t key = hash_filename(temp.filename);

  int found = btree_search(disk, handle, handle->root_block, key, result);
  free(handle);
  return found ? 0 : -1;
}

/**
 * @brief Reads the next directory entry in sorted (in-order) key order.
 *
 * Returns entries in ascending order of the directory entry key (hash of the
 * filename). Keeps iteration state inside @ref dir_t (iter_started, iter_key).
 * The @p handle parameter is unused.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock (unused).
 * @param dir Pointer to the open directory structure (dir_t).
 * @param handle Unused parameter; kept for interface compatibility.
 * @param out Output parameter to hold the directory entry read.
 * @return 0 if an entry was read successfully, -1 if end of directory is
 *         reached or on error.
 */
static int btree_find_min_entry(FILE *disk, btree_handle_t *handle,
                                uint32_t node_block, void *out_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;
  int ok;
  if (node->is_leaf) {
    if (node->num_entries > 0) {
      memcpy(out_entry, node->entries, handle->entry_size);
      ok = 1;
    } else {
      ok = 0;
    }
  } else {
    ok = btree_find_min_entry(disk, handle, node->children[0], out_entry);
  }
  btree_free_node(node);
  return ok;
}

static int btree_find_successor_entry(FILE *disk, btree_handle_t *handle,
                                      uint32_t node_block, uint32_t after_key,
                                      void *out_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;
  int found = 0;
  uint8_t *entries = node->entries;

  for (int i = 0; i < node->num_entries && !found; i++) {
    uint32_t ekey = handle->get_key(entries + i * handle->entry_size);
    if (ekey > after_key) {
      if (!node->is_leaf) {
        // Try to find in left child first
        if (btree_find_successor_entry(disk, handle, node->children[i],
                                       after_key, out_entry)) {
          found = 1;
          break;
        }
      }
      // Current entry is the successor
      memcpy(out_entry, entries + i * handle->entry_size, handle->entry_size);
      found = 1;
      break;
    }
  }

  // If not found, search in the rightmost child
  if (!found && !node->is_leaf) {
    found = btree_find_successor_entry(disk, handle,
                                       node->children[node->num_entries],
                                       after_key, out_entry);
  }

  btree_free_node(node);
  return found;
}

int read_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                   btree_handle_t *unused, dir_entry_t *out) {
  (void)sb;
  (void)unused;
  if (!disk || !dir || !out)
    return -1;

  if (dir->inode.btree_root == 0)
    return -1;

  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  int ok = 0;
  if (!dir->iter_started) {
    ok = btree_find_min_entry(disk, handle, handle->root_block, out);
    if (ok) {
      dir->iter_started = 1;
      dir->iter_key = hash_filename(out->filename);
    }
  } else {
    ok = btree_find_successor_entry(disk, handle, handle->root_block,
                                    dir->iter_key, out);
    if (ok) {
      dir->iter_key = hash_filename(out->filename);
    }
  }

  free(handle);
  return ok ? 0 : -1;
}
