/**
 * @file dir_entry_operations_bsfs.c
 * @brief Directory entry B-tree operations for BSFS.
 *
 * Provides the in-memory directory descriptor (@ref dir_t) and the set of
 * operations for creating, removing, updating, looking up, and iterating
 * directory entries backed by a B-tree stored in the filesystem image.
 */
#include "../../include/dir_entry_operations_bsfs.h"
#include "../../include/allocate_bsfs.h"

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
  btree_handle_t *dir_handle = btree_create_dir_entry_handle();
  dir_handle->root_block = dir->inode.btree_root;

  // Prevent duplicates by exact name, not by hash only
  dir_entry_t needle;
  memset(&needle, 0, sizeof(needle));
  strncpy(needle.filename, entry->filename, MAX_FILENAME - 1);
  needle.filename[MAX_FILENAME - 1] = '\0';

  dir_entry_t existing;
  if (btree_search_entry(disk, dir_handle, dir_handle->root_block, &needle,
                         &existing)) {
    fprintf(stderr, "Directory entry '%s' already exists.\n", entry->filename);
    free(dir_handle);
    return -1;
  }

  uint32_t old_root = dir_handle->root_block;

  if (btree_insert(disk, sb, dir_handle, entry) != 0) {
    free(dir_handle);
    return -1;
  }

  // Persist btree root changes (first insert or after a split)
  if (dir_handle->root_block != 0 &&
      dir_handle->root_block != dir->inode.btree_root) {
    fprintf(stderr,
            "[DIR ROOT] root changed inode=%u from %u to %u (persist)\n",
            dir->inode.inode_number, dir->inode.btree_root,
            dir_handle->root_block);
    dir->inode.btree_root = dir_handle->root_block;
    btree_handle_t *inode_handle = btree_create_inode_handle();
    inode_handle->root_block = sb->inode_root;
    (void)update_inode(disk, inode_handle, &dir->inode);
    free(inode_handle);
  }

  (void)old_root;

  dir->iter_started = 0;
  dir->iter_key = 0;
  free(dir_handle);
  return 0;
}

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
int scan_dir_tree_find(FILE *disk, btree_handle_t *handle,
                              uint32_t node_block, const char *name,
                              dir_entry_t *out) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  // Check current node entries first
  for (uint32_t i = 0; i < node->num_entries; i++) {
    dir_entry_t *e =
        (dir_entry_t *)((uint8_t *)node->entries + i * handle->entry_size);
    if (strncmp(e->filename, name, MAX_FILENAME) == 0) {
      memcpy(out, e, sizeof(dir_entry_t));
      btree_free_node(node);
      return 1;
    }
  }

  // Recurse into children if not a leaf
  int found = 0;
  if (!node->is_leaf) {
    for (uint32_t i = 0; i <= node->num_entries && !found; i++) {
      found = scan_dir_tree_find(disk, handle, node->children[i], name, out);
    }
  }

  btree_free_node(node);
  return found;
}

/**
 * @brief Collect all directory entries in-order into a dynamic array.
 */
/* Rebuild helpers removed: comparator-based deletion is used instead. */

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

  // Locate the exact entry by name first
  dir_entry_t found;
  int present = scan_dir_tree_find(disk, handle, handle->root_block, filename,
                                   &found);
  if (!present) {
    fprintf(stderr, "[DIR RM] entry '%s' not found (root=%u)\n", filename,
            handle->root_block);
    free(handle);
    return -1;
  }

  // Remove the exact entry using comparator-based B-tree deletion
  dir_entry_t needle;
  memset(&needle, 0, sizeof(needle));
  strncpy(needle.filename, filename, MAX_FILENAME - 1);
  needle.filename[MAX_FILENAME - 1] = '\0';
  uint32_t before_root = handle->root_block;
  int ok = btree_remove_entry(disk, sb, handle, &needle);
  if (!ok) {
    fprintf(stderr, "[DIR RM] btree_remove_entry failed for '%s' (root=%u)\n",
            filename, before_root);
    free(handle);
    return -1;
  }
  // Persist possible root change in the directory inode
  if (handle->root_block != before_root) {
    dir->inode.btree_root = handle->root_block;
    btree_handle_t *inode_h = btree_create_inode_handle();
    inode_h->root_block = sb->inode_root;
    (void)update_inode(disk, inode_h, &dir->inode);
    free(inode_h);
  }

  free(handle);
  return 0;
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
 * @return 1 on success; -1 if the entry was not found.
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
  (void)sb;
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  // Robust path: deep scan the directory B-tree to match exact name.
  // This avoids relying on strict key-order invariants after complex updates
  // (e.g., rebuilds/merges) and ensures correctness for exact lookups.
  int found = scan_dir_tree_find(disk, handle, handle->root_block, name, result);

  // Optional fast path (disabled): if needed in the future, try
  // btree_search_entry first and fall back to scan on miss.

  free(handle);
  return found ? 0 : -1;
}

/**
 * @brief Find the minimum-key directory entry in a subtree.
 *
 * Descends along the left-most children until reaching a leaf and returns the
 * first entry, which corresponds to the minimum key in that subtree.
 *
 * @param disk Pointer to the open disk file.
 * @param handle B-tree handle for directory entries.
 * @param node_block Block number of the subtree root.
 * @param out_entry Output buffer to receive the minimum entry.
 * @return 1 if an entry was found; 0 if the subtree is empty or on error.
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
      if (handle->type == BTREE_TYPE_DIR_ENTRY) {
        dir_entry_t *e = (dir_entry_t *)out_entry;
        fprintf(stderr, "[DIR ITER] min entry: %s (key=%u)\n", e->filename,
                hash_filename(e->filename));
      }
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

/**
 * @brief Find the in-order successor entry with key greater than after_key.
 *
 * Searches the subtree rooted at @p node_block for the smallest entry whose
 * key is strictly greater than @p after_key, copying it into @p out_entry.
 *
 * @param disk Pointer to the open disk file.
 * @param handle B-tree handle for directory entries.
 * @param node_block Root block of the subtree to search.
 * @param after_key Return the successor of this key.
 * @param out_entry Output buffer to receive the successor entry.
 * @return 1 if a successor exists; 0 otherwise.
 */
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

  for (uint32_t i = 0; i < node->num_entries && !found; i++) {
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
      if (handle->type == BTREE_TYPE_DIR_ENTRY) {
        dir_entry_t *e = (dir_entry_t *)out_entry;
        fprintf(stderr, "[DIR ITER] succ after %u -> %s (key=%u)\n", after_key,
                e->filename, hash_filename(e->filename));
      }
      found = 1;
      break;
    }
  }

  // If not found, search in the rightmost child
  if (!found && !node->is_leaf) {
    found = btree_find_successor_entry(
        disk, handle, node->children[node->num_entries], after_key, out_entry);
  }

  btree_free_node(node);
  return found;
}

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
                   btree_handle_t *handle, dir_entry_t *out) {
  (void)sb;
  (void)handle;
  if (!disk || !dir || !out)
    return -1;

  if (dir->inode.btree_root == 0)
    return -1;

  btree_handle_t *dir_handle = btree_create_dir_entry_handle();
  dir_handle->root_block = dir->inode.btree_root;

  int ok = 0;
  if (!dir->iter_started) {
    ok = btree_find_min_entry(disk, dir_handle, dir_handle->root_block, out);
    if (ok) {
      dir->iter_started = 1;
      dir->iter_key = hash_filename(out->filename);
      fprintf(stderr, "[DIR ITER] start key=%u (%s)\n", dir->iter_key,
              out->filename);
    }
  } else {
    ok = btree_find_successor_entry(disk, dir_handle, dir_handle->root_block,
                                    dir->iter_key, out);
    if (ok) {
      fprintf(stderr, "[DIR ITER] next from key=%u -> %u (%s)\n", dir->iter_key,
              hash_filename(out->filename), out->filename);
      dir->iter_key = hash_filename(out->filename);
    }
  }

  free(dir_handle);
  if (!ok) {
    dir->iter_started = 0;
    dir->iter_key = 0;
    return -1;
  }
  return 0;
}
