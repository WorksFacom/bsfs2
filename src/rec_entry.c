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

#include "../include/rec_entry.h"
#include <stdio.h>

/*
 * Maintain a monotonic recovery-id counter in memory. We derive its initial
 * value by scanning the recovery tree once per root change. This avoids
 * repeated full-tree scans on every insert and, more importantly, prevents the
 * tree from ever being traversed in an inconsistent state during id
 * assignment (which previously caused invalid memory reads).
 */
static uint32_t s_next_recovery_id     = 1;
static uint32_t s_cached_recovery_root = INVALID_BLOCK;

static int rec_tree_find_max_entry(FILE *disk, btree_handle_t *handle, uint32_t node_block, rec_entry_t *out) {
  if (!handle || node_block == 0 || !out)
    return 0;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  int ok = 0;
  if (node->is_leaf) {
    if (node->num_entries > 0) {
      memcpy(out, (uint8_t *)node->entries + (node->num_entries - 1) * handle->entry_size, handle->entry_size);
      ok = 1;
    }
  } else {
    uint32_t child_index = node->num_entries;
    if (child_index >= (uint32_t)(2 * handle->min_degree)) {
      child_index = (node->num_entries > 0) ? node->num_entries - 1 : 0;
    }
    uint32_t child_block = node->children[child_index];
    if (child_block != 0 && child_block != INVALID_BLOCK) {
      ok = rec_tree_find_max_entry(disk, handle, child_block, out);
    }
    if (!ok && node->num_entries > 0) {
      memcpy(out, (uint8_t *)node->entries + (node->num_entries - 1) * handle->entry_size, handle->entry_size);
      ok = 1;
    }
  }

  btree_free_node(node);
  return ok;
}

static void rec_tree_sync_next_id(FILE *disk, btree_handle_t *handle) {
  if (!handle)
    return;

  if (handle->root_block == 0) {
    s_next_recovery_id     = 1;
    s_cached_recovery_root = 0;
    return;
  }

  if (s_cached_recovery_root == handle->root_block)
    return;

  rec_entry_t max_entry;
  if (rec_tree_find_max_entry(disk, handle, handle->root_block, &max_entry)) {
    s_next_recovery_id = max_entry.recovery_id + 1;
  } else {
    s_next_recovery_id = 1;
  }
  s_cached_recovery_root = handle->root_block;
}

uint32_t get_next_rec_id(FILE *disk, bsfs_super_block *sb, btree_handle_t *rec_tree) {
  if (!disk || !sb || !rec_tree)
    return s_next_recovery_id++;

  rec_tree_sync_next_id(disk, rec_tree);
  if (rec_tree->root_block != s_cached_recovery_root)
    rec_tree_sync_next_id(disk, rec_tree);

  return s_next_recovery_id++;
}

int rec_tree_remove(FILE *disk, bsfs_super_block *sb, btree_handle_t *rec_tree, uint32_t recovery_id) {
  if (!disk || !sb || !rec_tree) {
    return 0;
  }
  rec_entry_t entry;
  if (!btree_search(disk, rec_tree, rec_tree->root_block, recovery_id, &entry)) {
    return 0;
  }

  int removed = btree_remove_entry(disk, sb, rec_tree, &entry);
  if (removed && sb->recovery_root != rec_tree->root_block) {
    sb->recovery_root = rec_tree->root_block;
    (void)write_superblock(disk, sb);
  }
  return removed ? 1 : 0;
}

int rec_tree_search(FILE *disk, btree_handle_t *rec_tree, uint32_t recovery_id, rec_entry_t *out_entry) {
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

int rec_tree_init(FILE *disk, bsfs_super_block *sb, btree_handle_t *out) {
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
    node.is_leaf     = 1;
    node.num_entries = 0;
    node.entries     = calloc((size_t)(2 * tmp->min_degree - 1), (size_t)tmp->entry_size);
    node.children    = calloc((size_t)(2 * tmp->min_degree), sizeof(uint32_t));
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
  rec_tree_sync_next_id(disk, tmp);
  *out = *tmp;
  free(tmp);
  return 1;
}

int rec_tree_add(FILE *disk, bsfs_super_block *sb, btree_handle_t *rec_tree, rec_entry_t *entry) {
  if (!disk || !sb || !rec_tree || !entry)
    return 0;

  // Assign a new recovery_id if missing
  if (entry->recovery_id == 0)
    entry->recovery_id = get_next_rec_id(disk, sb, rec_tree);

  // Compute default retention if not provided
  if (entry->retention_until == 0 && entry->deleted_at != 0) {
    entry->retention_until = entry->deleted_at + DEFAULT_RECOVERY_TTL;
  }

  int attempts = 0;
  int ok       = -1;
  while (ok != 0 && attempts < 8) {
    ok = btree_insert(disk, sb, rec_tree, entry);
    if (ok == 0)
      break;
    entry->recovery_id = get_next_rec_id(disk, sb, rec_tree);
    attempts++;
  }
  if (ok != 0)
    return 0;

  if (entry->recovery_id >= s_next_recovery_id)
    s_next_recovery_id = entry->recovery_id + 1;

  // Persist Recovery root whenever it changes (e.g., after a split creating a
  // new root). This ensures future sessions/loaders see the correct root.
  if (rec_tree->root_block != 0 && sb->recovery_root != rec_tree->root_block) {
    sb->recovery_root = rec_tree->root_block;
    (void)write_superblock(disk, sb);
    s_cached_recovery_root = rec_tree->root_block;
  }

  return 1;
}

static int rec_tree_collect_by_name(FILE *disk, btree_handle_t *handle, uint32_t node_block, const char *name, rec_entry_t *out, size_t max,
                                    size_t *count) {
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  rec_entry_t *entries = (rec_entry_t *)node->entries;
  int          ok      = 1;

  for (uint32_t i = 0; i < node->num_entries && ok; i++) {
    if (!node->is_leaf) {
      ok = rec_tree_collect_by_name(disk, handle, node->children[i], name, out, max, count);
    }

    if (ok && strcmp(entries[i].original_name, name) == 0) {
      if (out && *count < max) {
        out[*count] = entries[i];
      }
      (*count)++;
    }
  }

  if (ok && !node->is_leaf) {
    ok = rec_tree_collect_by_name(disk, handle, node->children[node->num_entries], name, out, max, count);
  }

  btree_free_node(node);
  return ok;
}

int rec_tree_find_by_name(FILE *disk, btree_handle_t *rec_tree, const char *name, rec_entry_t *out, size_t max, size_t *out_count) {
  if (!disk || !rec_tree || !name || !out_count)
    return 0;

  *out_count = 0;
  if (rec_tree->root_block == 0)
    return 1; /* empty tree */

  size_t cap = out ? max : 0;
  return rec_tree_collect_by_name(disk, rec_tree, rec_tree->root_block, name, out, cap, out_count);
}

int purge_file(FILE *disk, bsfs_super_block *sb, btree_handle_t *data_tree, btree_handle_t *rec_tree, const rec_entry_t *rec) {
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
      fprintf(stderr, "purge_file: failed deallocating direct span rid=%u disk=%u len=%u\n", rec->recovery_id, sp->disk_blk, sp->length);
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
        if (!btree_remove_entry(disk, sb, data_tree, &found)) {
          fprintf(stderr, "purge_file: btree_remove failed rid=%u logical=%u\n", rec->recovery_id, found.file_blk);
          return -1;
        }
      }

      /* Return the physical extent to the free space map. */
      if (deallocate_bspan(disk, sb, &found) != 0) {
        fprintf(stderr, "purge_file: failed deallocating tree span rid=%u disk=%u len=%u\n", rec->recovery_id, found.disk_blk,
                found.length);
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
  if (!btree_remove_entry(disk, sb, rec_tree, rec)) {
    fprintf(stderr, "purge_file: failed removing recovery entry rid=%u\n", rec->recovery_id);
    return -1;
  }

  if (sb->recovery_root != rec_tree->root_block) {
    sb->recovery_root = rec_tree->root_block;
    (void)write_superblock(disk, sb);
  }

  return 0;
}
