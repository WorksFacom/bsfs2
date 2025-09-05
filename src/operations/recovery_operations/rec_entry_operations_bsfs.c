/**
 * @file rec_entry_operations_bsfs.c
 * @brief Recovery entry B-tree operations for BSFS (implementation).
 *
 * This module implements the management of the Recovery B-tree, which stores
 * snapshots of deleted files (rec_entry_t) to enable later restoration.
 * It provides initialization of the recovery tree, CRUD-like operations
 * (add/remove/search), and a purge routine based on a timestamp threshold.
 *
 * Notes:
 *  - Data block reclamation for purged entries must be performed by a separate
 *    garbage-collection component, which can iterate the removed entries and
 *    free their blocks safely.
 *  - The B-tree handle provided here is type-agnostic; key encoding and
 *    comparison are adapted for the rec_entry key layout.
 */

#include "../../include/rec_entry_operations_bsfs.h"
#include <string.h>

/**
 * @brief Finds the next available recovery ID number by scanning the recovery
 * B-tree.
 *
 * This function scans the recovery tree to find the highest recovery ID in use
 * and returns the next available one. This ensures uniqueness of recovery ID
 * without reusing deleted ones.
 *
 * @param disk Pointer to the disk file.
 * @param inode_tree Pointer to the B-tree handle for recovery tree.
 * @return The next available (unused) recovery ID.
 */
uint32_t get_next_rec_id(FILE *disk, btree_handle_t *rec_tree) {
  if (!rec_tree || rec_tree->root_block == 0)
    return 1;

  uint32_t max_rec_id = 0;
  btree_node_t *node = btree_read_node(disk, rec_tree, rec_tree->root_block);

  while (!node->is_leaf) {
    uint32_t last_child = node->children[node->num_entries];
    btree_free_node(node);
    node = btree_read_node(disk, rec_tree, last_child);
  }

  if (node->num_entries > 0) {
    rec_entry_t *entries = (rec_entry_t *)node->entries;
    rec_entry_t *last = &entries[node->num_entries - 1];
    max_rec_id = last->recovery_id;
  }

  btree_free_node(node);
  return max_rec_id + 1;
}

/**
 * @brief Remove a recovery entry from the Recovery B-tree by ID.
 *
 * This function deletes a single @ref rec_entry_t identified by its
 * @p recovery_id from the Recovery tree. The B-tree for recovery entries
 * must be configured so that the key is the 32-bit @c recovery_id field of
 * @ref rec_entry_t (i.e., handle->get_key returns &entry->recovery_id).
 *
 * Semantics:
 *  - If an entry with the given @p recovery_id exists, it is removed from
 *    the tree and the function returns 1.
 *  - If no such entry exists, the function returns 0.
 *
 * Notes:
 *  - This function only removes the metadata entry from the Recovery B-tree.
 *    Reclaiming data blocks referenced by the entry (if any) must be handled
 *    by a higher-level GC routine before or after removal, depending on your
 *    chosen policy.
 *
 * @param disk       Open filesystem image.
 * @param sb         Loaded superblock.
 * @param rec_tree   Recovery B-tree handle (already initialized).
 * @param recovery_id Unique identifier of the recovery entry to remove.
 * @return 1 if the entry was found and removed; 0 otherwise.
 */
int rec_tree_remove(FILE *disk, superblock_t *sb, btree_handle_t *rec_tree,
                    uint32_t recovery_id) {
  if (!disk || !sb || !rec_tree) {
    return 0;
  }
  return btree_remove(disk, sb, rec_tree, recovery_id) ? 1 : 0;
}

/**
 * @brief Look up a recovery entry in the Recovery B-tree by ID.
 *
 * This function searches the Recovery B-tree for a @ref rec_entry_t whose
 * key is the 32-bit @p recovery_id. The B-tree must be configured so that
 * the key for each node record is the @c recovery_id field of @ref rec_entry_t
 * (i.e., the handle's key accessor returns &entry->recovery_id).
 *
 * Semantics:
 *  - If an entry with the given @p recovery_id exists, it is copied into
 *    @p out_entry and the function returns 1.
 *  - If not found, the function returns 0 and leaves @p out_entry unmodified.
 *
 * @param disk        Open filesystem image.
 * @param rec_tree    Recovery B-tree handle (already initialized).
 * @param recovery_id Key (unique ID) of the recovery entry to find.
 * @param out_entry   Output buffer to receive the found entry (must be
 * non-NULL).
 * @return 1 if found; 0 if not found or on invalid arguments.
 */
int rec_tree_search(FILE *disk, btree_handle_t *rec_tree, uint32_t recovery_id,
                    rec_entry_t *out_entry) {
  if (!disk || !rec_tree || !out_entry) {
    return 0;
  }

  rec_entry_t tmp;
  if (btree_search(disk, rec_tree, rec_tree->root_block, recovery_id, &tmp)) {
    *out_entry = tmp;
    return 1;
  }
  return 0;
}

/**
 * @brief Initialize (or load) the Recovery B-tree handle.
 *
 * This function prepares a recovery B-tree handle. If the recovery root
 * does not exist yet (sb->recovery_root == 0), it allocates a fresh root
 * node on disk, initializes it as an empty leaf, persists the node, and
 * stores the new root block in both the handle and the superblock.
 *
 * If the root already exists, the handle is simply configured to point to it.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (updated if a new root is created).
 * @param out  Output: initialized recovery B-tree handle (by value).
 * @return 1 on success; 0 on failure.
 */
int rec_tree_init(FILE *disk, superblock_t *sb, btree_handle_t *out) {
  if (!disk || !sb || !out)
    return 0;

  btree_handle_t *tmp = btree_create_rec_entry_handle();
  if (!tmp)
    return 0;

  if (sb->recovery_root == 0) {
    // Allocate a new root block and initialize an empty leaf node
    uint32_t root_block = allocate_single_block(disk, sb);
    if (root_block == INVALID_BLOCK || root_block == 0) {
      free(tmp);
      return 0;
    }

    tmp->root_block = root_block;

    btree_node_t node;
    node.is_leaf = 1;
    node.num_entries = 0;
    node.entries =
        calloc((size_t)(2 * tmp->min_degree - 1), (size_t)tmp->entry_size);
    node.children = calloc((size_t)(2 * tmp->min_degree), sizeof(uint32_t));
    if (!node.entries || !node.children) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }

    if (btree_write_node(disk, tmp, root_block, &node) != 0) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }

    // Clean up temporary node buffers
    free(node.entries);
    free(node.children);

    // Persist new recovery root in the superblock
    sb->recovery_root = root_block;
    if (write_superblock(disk, sb) != 0) {
      free(tmp);
      return 0;
    }
  } else {
    // Root exists already; just point the handle to it
    tmp->root_block = sb->recovery_root;
  }

  // Return the handle by value and free the temporary heap allocation
  *out = *tmp;
  free(tmp);
  return 1;
}

/**
 * @brief Insert a recovery entry into the Recovery B-tree.
 *
 * Assigns a fresh @c recovery_id if the input entry does not carry one
 * yet, and sets a default @c retention_until based on @c deleted_at if
 * not provided. Then inserts the entry into the recovery B-tree.
 *
 * If this is the first insertion and the superblock has not yet persisted
 * @c recovery_root, this function will update and persist it.
 *
 * @param disk     Open filesystem image.
 * @param sb       Loaded superblock (may be updated to persist recovery_root).
 * @param rec_tree Recovery B-tree handle (already initialized).
 * @param entry    In/out entry to insert (recovery_id may be assigned).
 * @return 1 on success; 0 on failure.
 */
int rec_tree_add(FILE *disk, superblock_t *sb, btree_handle_t *rec_tree,
                 rec_entry_t *entry) {
  if (!disk || !sb || !rec_tree || !entry)
    return 0;

  // Assign a new recovery_id if missing
  if (entry->recovery_id == 0) {
    entry->recovery_id = get_next_rec_id(disk, rec_tree);
    if (entry->recovery_id == 0) {
      return 0;
    }
  }

  // Compute default retention if not provided
  if (entry->retention_until == 0 && entry->deleted_at != 0) {
    entry->retention_until = entry->deleted_at + DEFAULT_RECOVERY_TTL;
  }

  // Insert the entry into the B-tree
  int ok = btree_insert(disk, sb, rec_tree, entry);

  // Best-effort: if we just created a root and the SB has not persisted it yet
  if (ok && sb->recovery_root == 0 && rec_tree->root_block != 0) {
    sb->recovery_root = rec_tree->root_block;
    (void)write_superblock(disk, sb);
  }

  return ok;
}

/**
 * @brief Helper: in-order traversal collecting Recovery entries that match
 *        a given name.
 *
 * Traverses the B-tree at @p node_block and compares each entry's
 * rec_entry_t::original_name with @p name. Up to @p max matches are copied
 * to @p out. The running total number of matches is accumulated in @p count.
 *
 * @param disk       Open filesystem image.
 * @param handle     Recovery B-tree handle.
 * @param node_block Block number of the current node.
 * @param name       Filename to match exactly.
 * @param out        Output buffer for matches (may be NULL to only count).
 * @param max        Capacity of @p out buffer.
 * @param count      In/out: total matches found so far.
 * @return 1 on success, 0 on error (e.g., failed to read a node).
 */
static int rec_tree_collect_by_name(FILE *disk, btree_handle_t *handle,
                                    uint32_t node_block, const char *name,
                                    rec_entry_t *out, size_t max,
                                    size_t *count) {
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  rec_entry_t *entries = (rec_entry_t *)node->entries;
  int ok = 1;

  for (int i = 0; i < node->num_entries && ok; i++) {
    if (!node->is_leaf) {
      ok = rec_tree_collect_by_name(disk, handle, node->children[i], name, out,
                                    max, count);
    }

    if (ok && strcmp(entries[i].original_name, name) == 0) {
      if (out && *count < max) {
        out[*count] = entries[i];
      }
      (*count)++;
    }
  }

  if (ok && !node->is_leaf) {
    ok = rec_tree_collect_by_name(
        disk, handle, node->children[node->num_entries], name, out, max, count);
  }

  btree_free_node(node);
  return ok;
}

/**
 * @brief Find recovery entries by original filename.
 *
 * Performs an in-order traversal over the Recovery B-tree and collects entries
 * whose rec_entry_t::original_name matches exactly the provided @p name.
 * Up to @p max matches are copied into @p out (if non-NULL). The total number
 * of matches (which can exceed @p max) is written to @p out_count.
 *
 * @param disk       Open filesystem image.
 * @param rec_tree   Initialized Recovery B-tree handle.
 * @param name       Exact filename to match against rec_entry_t::original_name.
 * @param out        Output buffer for matches (may be NULL to only count).
 * @param max        Capacity of @p out in number of entries.
 * @param out_count  Output: total number of matches found.
 * @return 1 on success (including zero matches), 0 on invalid args or I/O
 *         failure while traversing.
 */
int rec_tree_find_by_name(FILE *disk, btree_handle_t *rec_tree,
                          const char *name, rec_entry_t *out, size_t max,
                          size_t *out_count) {
  if (!disk || !rec_tree || !name || !out_count)
    return 0;

  *out_count = 0;
  if (rec_tree->root_block == 0)
    return 1; /* empty tree */

  size_t cap = out ? max : 0;
  return rec_tree_collect_by_name(disk, rec_tree, rec_tree->root_block, name,
                                  out, cap, out_count);
}

/**
 * @brief Permanently purge a deleted file from the filesystem.
 *
 * This routine performs a full cleanup of a file that was previously deleted
 * and tracked by the Recovery tree. It:
 *   1) Deallocates all data blocks referenced by the file (direct spans and
 *      spans in the per-file data B-tree, if present);
 *   2) Clears the file's data B-tree mappings (removing span records);
 *   3) Removes the corresponding recovery entry from the Recovery B-tree.
 *
 * Failure semantics:
 *  - If deallocation of data or removal of span mappings fails, the recovery
 *    entry is NOT removed, and the function returns -1. This preserves a handle
 *    to retry or inspect the failure later.
 *  - Only when the data has been fully reclaimed will the recovery entry be
 *    removed.
 *
 * Requirements/assumptions:
 *  - @p rec contains a complete snapshot (direct spans, @c btree_root, size).
 *  - @p data_tree is a B-tree handle configured to operate on file data spans
 *    (the same type used for regular file block mappings).
 *  - The @c btree_search / @c btree_remove APIs accept an explicit root block
 *    parameter to operate on the per-file data tree (see calls below). If your
 *    project’s signatures differ, adapt the calls accordingly.
 *
 * @param disk      Open filesystem image.
 * @param sb        Loaded superblock.
 * @param data_tree B-tree handle for file data spans (bspan_t mappings).
 * @param rec_tree  B-tree handle for recovery entries (rec_entry_t by key
 * recovery_id).
 * @param rec       The recovery entry describing the file to purge.
 * @return 0 on success; -1 on error.
 */
int purge_file(FILE *disk, superblock_t *sb, btree_handle_t *data_tree,
               btree_handle_t *rec_tree, const rec_entry_t *rec) {
  if (!disk || !sb || !data_tree || !rec_tree || !rec) {
    return -1;
  }

  /* -------------------------
   * 1) Deallocate direct spans
   * ------------------------- */
  for (int i = 0; i < 16; i++) {
    const bspan_t *sp = &rec->direct_blocks[i];
    if (sp->length == 0)
      continue;

    /* Return the contiguous run of physical blocks to the free space map. */
    if (deallocate_bspan(disk, sb, sp) != 0) {
      return -1;
    }
  }

  /* ---------------------------------------------
   * 2) Deallocate spans from the per-file data B-tree
   *    and clear the span mappings from that tree.
   * --------------------------------------------- */
  if (rec->btree_root != 0) {
    /* Logical end (exclusive) derived from the file size. */
    const uint32_t block_count = (rec->file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* Walk the logical space from 0..block_count-1, but remove whole spans
       at a time. We always remove by the SPAN START key (bspan.file_blk). */
    uint32_t logical = 0;

    while (logical < block_count) {
      bspan_t found;

      if (!btree_search(disk, data_tree, rec->btree_root, logical, &found)) {
        logical++;
        continue;
      }

      /* Remove the span mapping using its starting logical block as key. */
      /* If your btree_remove signature does not accept root, adapt here. */
      if (!btree_remove(disk, sb, data_tree, found.file_blk)) {
        return -1;
      }

      /* Return the physical extent to the free space map. */
      if (deallocate_bspan(disk, sb, &found) != 0) {
        return -1;
      }

      /* Jump past the removed span. */
      uint32_t next = found.file_blk + found.length;
      if (next <= logical) {
        logical++;
      } else {
        logical = next;
      }
    }
  }

  /* ---------------------------------------------------
   * 3) Remove the recovery entry (metadata) from the tree
   * --------------------------------------------------- */
  if (!btree_remove(disk, sb, rec_tree, rec->recovery_id)) {
    return -1;
  }

  return 0;
}
