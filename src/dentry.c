/**
 * @file dir_entry_operations_bsfs.c
 * @brief Directory entry B-tree operations for BSFS.
 *
 * Provides the in-memory directory descriptor (@ref dir_t) and the set of
 * operations for creating, removing, updating, looking up, and iterating
 * directory entries backed by a B-tree stored in the filesystem image.
 */
#include "../include/dentry.h"
#include "../include/alloc.h"
#include "../include/btree.h"
#include "../include/inode.h"

int add_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, dir_entry_t *entry) {
  btree_handle_t *dir_handle = btree_create_dir_entry_handle();
  dir_handle->root_block     = dir->inode.btree_root;

  // Prevent duplicates by exact name, not by hash only
  dir_entry_t needle;
  memset(&needle, 0, sizeof(needle));
  strncpy(needle.filename, entry->filename, MAX_FILENAME - 1);
  needle.filename[MAX_FILENAME - 1] = '\0';

  dir_entry_t existing;
  if (btree_search_entry(disk, dir_handle, dir_handle->root_block, &needle, &existing)) {
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
  if (dir_handle->root_block != 0 && dir_handle->root_block != dir->inode.btree_root) {
    dir->inode.btree_root        = dir_handle->root_block;
    btree_handle_t *inode_handle = btree_create_inode_handle();
    inode_handle->root_block     = sb->inode_root;
    (void)update_inode(disk, inode_handle, &dir->inode);
    free(inode_handle);
  }

  (void)old_root;

  dir->iter_started = 0;
  dir->iter_key     = 0;
  free(dir_handle);
  return 0;
}

int scan_dir_tree_find(FILE *disk, btree_handle_t *handle, uint32_t node_block, const char *name, dir_entry_t *out) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  // Check current node entries first
  for (uint32_t i = 0; i < node->num_entries; i++) {
    dir_entry_t *e = (dir_entry_t *)((uint8_t *)node->entries + i * handle->entry_size);
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

/* Rebuild helpers removed: comparator-based deletion is used instead. */

int remove_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, const char *filename) {
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block     = dir->inode.btree_root;

  // Locate the exact entry by name first
  dir_entry_t found;
  int         present = scan_dir_tree_find(disk, handle, handle->root_block, filename, &found);
  if (!present) {
    free(handle);
    return -1;
  }

  // Remove the exact entry using comparator-based B-tree deletion
  dir_entry_t needle;
  memset(&needle, 0, sizeof(needle));
  strncpy(needle.filename, filename, MAX_FILENAME - 1);
  needle.filename[MAX_FILENAME - 1] = '\0';
  uint32_t before_root              = handle->root_block;
  int      ok                       = btree_remove_entry(disk, sb, handle, &needle);
  if (!ok) {
    free(handle);
    return -1;
  }
  // Persist possible root change in the directory inode
  if (handle->root_block != before_root) {
    dir->inode.btree_root   = handle->root_block;
    btree_handle_t *inode_h = btree_create_inode_handle();
    inode_h->root_block     = sb->inode_root;
    (void)update_inode(disk, inode_h, &dir->inode);
    free(inode_h);
  }

  free(handle);
  return 0;
}

int update_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, dir_entry_t *entry) {
  (void)sb;
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block     = dir->inode.btree_root;

  uint32_t hash = hash_filename(entry->filename);

  dir_entry_t existing;
  if (!btree_search(disk, handle, handle->root_block, hash, &existing)) {
    fprintf(stderr, "Cannot update: entry '%s' not found in directory.\n", entry->filename);
    free(handle);
    return -1;
  }

  int ok = btree_update(disk, handle, handle->root_block, entry);
  free(handle);
  return ok;
}

int lookup_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, const char *name, dir_entry_t *result) {
  (void)sb;
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block     = dir->inode.btree_root;

  // Robust path: deep scan the directory B-tree to match exact name.
  // This avoids relying on strict key-order invariants after complex updates
  // (e.g., rebuilds/merges) and ensures correctness for exact lookups.
  int found = scan_dir_tree_find(disk, handle, handle->root_block, name, result);

  // Optional fast path (disabled): if needed in the future, try
  // btree_search_entry first and fall back to scan on miss.

  free(handle);
  return found ? 0 : -1;
}

static int btree_find_min_entry(FILE *disk, btree_handle_t *handle, uint32_t node_block, void *out_entry) {
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

static int btree_find_successor_entry(FILE *disk, btree_handle_t *handle, uint32_t node_block, uint32_t after_key, void *out_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;
  int      found   = 0;
  uint8_t *entries = node->entries;

  for (uint32_t i = 0; i < node->num_entries && !found; i++) {
    uint32_t ekey = handle->get_key(entries + i * handle->entry_size);
    if (ekey > after_key) {
      if (!node->is_leaf) {
        // Try to find in left child first
        if (btree_find_successor_entry(disk, handle, node->children[i], after_key, out_entry)) {
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
    found = btree_find_successor_entry(disk, handle, node->children[node->num_entries], after_key, out_entry);
  }

  btree_free_node(node);
  return found;
}

int read_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, btree_handle_t *handle, dir_entry_t *out) {
  (void)sb;
  (void)handle;
  if (!disk || !dir || !out)
    return -1;

  if (dir->inode.btree_root == 0)
    return -1;

  btree_handle_t *dir_handle = btree_create_dir_entry_handle();
  dir_handle->root_block     = dir->inode.btree_root;

  int ok = 0;
  if (!dir->iter_started) {
    ok = btree_find_min_entry(disk, dir_handle, dir_handle->root_block, out);
    if (ok) {
      dir->iter_started = 1;
      dir->iter_key     = hash_filename(out->filename);
    }
  } else {
    ok = btree_find_successor_entry(disk, dir_handle, dir_handle->root_block, dir->iter_key, out);
    if (ok) {
      dir->iter_key = hash_filename(out->filename);
    }
  }

  free(dir_handle);
  if (!ok) {
    dir->iter_started = 0;
    dir->iter_key     = 0;
    return -1;
  }
  return 0;
}
