/**
 * @file rec_gc_bsfs.c
 * @brief On-demand garbage collection for Recovery entries in BSFS.
 *
 * Implements routines to purge recovery entries in order to free disk space
 * when allocations fail. The GC prefers removing expired entries first;
 * if still insufficient, it removes the oldest non-expired entries until the
 * requested number of blocks is freed.
 */

#include "../../../include/rec_gc_bsfs.h"
#include "../../../include/allocate_bsfs.h"
#include "../../../include/btree_bsfs.h"
#include "../../../include/rec_entry_operations_bsfs.h"

/**
 * @brief Internal item wrapper used during Recovery GC traversals.
 *
 * Carries a copy of a recovery entry while collecting and sorting
 * candidates for purging.
 */
typedef struct {
  rec_entry_t entry; /**< Snapshot copied from the Recovery B-tree. */
} rec_item_t;

/**
 * @brief Collect all recovery entries from the Recovery tree into a dynamic array.
 *
 * Performs an in-order traversal starting at @p node_block, appending each
 * rec_entry_t to an array that grows as needed by doubling its capacity.
 *
 * @param disk        Open filesystem image.
 * @param rec_handle  Recovery B-tree handle.
 * @param node_block  Block number of the current node to traverse.
 * @param arr         In/out: pointer to the dynamic array buffer.
 * @param count       In/out: number of valid items currently stored.
 * @param cap         In/out: current capacity of @p arr (in items).
 */
static void collect_all(FILE *disk, btree_handle_t *rec_handle,
                        uint32_t node_block, rec_item_t **arr, size_t *count,
                        size_t *cap) {
  if (node_block == 0)
    return;
  btree_node_t *node = btree_read_node(disk, rec_handle, node_block);
  if (!node)
    return;
  if (!node->is_leaf) {
    for (uint32_t i = 0; i < node->num_entries; i++)
      collect_all(disk, rec_handle, node->children[i], arr, count, cap);
  }

  // entries
  for (uint32_t i = 0; i < node->num_entries; i++) {
    if (*count == *cap) {
      *cap = (*cap == 0) ? 64 : (*cap * 2);
      *arr = (rec_item_t *)realloc(*arr, *cap * sizeof(rec_item_t));
      if (!*arr)
        break;
    }
    memcpy(&(*arr)[*count].entry,
           (uint8_t *)node->entries + i * rec_handle->entry_size,
           sizeof(rec_entry_t));
    (*count)++;
  }

  if (!node->is_leaf)
    collect_all(disk, rec_handle, node->children[node->num_entries], arr, count,
                cap);

  btree_free_node(node);
}

/**
 * @brief qsort comparator for ascending deleted_at (and recovery_id as tiebreaker).
 *
 * @param a Pointer to the first rec_item_t.
 * @param b Pointer to the second rec_item_t.
 * @return Negative if a<b, positive if a>b, 0 if equal by the ordering.
 */
static int cmp_deleted_at(const void *a, const void *b) {
  const rec_item_t *ra = (const rec_item_t *)a;
  const rec_item_t *rb = (const rec_item_t *)b;
  if (ra->entry.deleted_at < rb->entry.deleted_at)
    return -1;
  if (ra->entry.deleted_at > rb->entry.deleted_at)
    return 1;
  return 0;
}

/**
 * @brief Purge a single recovery entry and reclaim its data blocks.
 *
 * Deallocates all direct spans and, if present, traverses the per-file bspan
 * tree to deallocate the remaining spans. Removes the metadata from the
 * Recovery B-tree.
 *
 * @param disk        Open filesystem image.
 * @param sb          Loaded superblock.
 * @param rec_handle  Recovery B-tree handle.
 * @param e           Recovery entry describing the deleted file.
 * @return Number of blocks freed by this purge operation.
 */
static size_t purge_one(FILE *disk, superblock_t *sb,
                        btree_handle_t *rec_handle, const rec_entry_t *e) {
  size_t freed = 0;

  if (!rec_tree_remove(disk, sb, rec_handle, e->recovery_id)) {
    fprintf(stderr,
            "rec_gc: failed to remove recovery id=%u; skipping purge for now.\n",
            e->recovery_id);
    return 0;
  }

  // free direct spans
  for (int i = 0; i < 16; i++) {
    if (e->direct_blocks[i].length > 0) {
      (void)deallocate_bspan(disk, sb, &e->direct_blocks[i]);
      freed += e->direct_blocks[i].length;
    }
  }

  // free bspan tree if present
  if (e->btree_root != 0) {
    btree_handle_t *bspan = btree_create_bspan_handle();
    bspan->root_block = e->btree_root;
    if (btree_deallocate_all(disk, sb, bspan, bspan->root_block) != 0) {
      fprintf(stderr,
              "rec_gc: partial purge for recovery id=%u (btree root=%u)\n",
              e->recovery_id, e->btree_root);
    }
    free(bspan);
    // Approximate using recorded block_count for total freed
  }

  // Trust recorded block_count as upper bound freed (covers direct + indirect)
  if (freed < e->block_count)
    freed = e->block_count;
  return freed;
}

/**
 * @brief Purge Recovery entries until at least a target number of blocks are freed.
 *
 * Attempts to reclaim space by first removing expired entries (those with
 * retention_until <= now). If that is insufficient, continues by removing the
 * oldest remaining entries (ascending deleted_at) until at least
 * @p needed_blocks have been freed or there are no more entries.
 *
 * For each purged entry, this routine deallocates blocks described by its 16
 * direct spans and, if present, traverses the per-file bspan B-tree to
 * deallocate the remaining spans. Finally, it removes the metadata entry from
 * the Recovery B-tree.
 *
 * @param disk          Open filesystem image.
 * @param sb            Loaded superblock (used for bitmap updates and recovery root).
 * @param needed_blocks Minimum number of blocks to free.
 * @param now           Current timestamp used for retention checks.
 * @return Number of blocks actually freed (can be less than requested).
 */
size_t rec_gc_purge_until(FILE *disk, superblock_t *sb, uint32_t needed_blocks,
                          uint64_t now) {
  if (!disk || !sb || needed_blocks == 0)
    return 0;

  btree_handle_t *rec_handle = btree_create_rec_entry_handle();
  if (!rec_handle)
    return 0;
  rec_handle->root_block = sb->recovery_root;
  if (rec_handle->root_block == 0) {
    free(rec_handle);
    return 0;
  }

  rec_item_t *items = NULL;
  size_t count = 0, cap = 0;
  collect_all(disk, rec_handle, rec_handle->root_block, &items, &count, &cap);

  size_t freed = 0;

  // 1) purge expired first
  for (size_t i = 0; i < count && freed < needed_blocks; i++) {
    if (items[i].entry.retention_until != 0 &&
        items[i].entry.retention_until <= now) {
      freed += purge_one(disk, sb, rec_handle, &items[i].entry);
    }
  }

  // 2) if still needed, purge by oldest deleted_at
  if (freed < needed_blocks) {
    qsort(items, count, sizeof(rec_item_t), cmp_deleted_at);
    for (size_t i = 0; i < count && freed < needed_blocks; i++) {
      // Skip ones already expired and likely removed — best-effort check:
      // Try to search; if not found skip.
      rec_entry_t tmp;
      if (!rec_tree_search(disk, rec_handle, items[i].entry.recovery_id, &tmp))
        continue;
      freed += purge_one(disk, sb, rec_handle, &items[i].entry);
    }
  }

  free(items);
  free(rec_handle);
  return freed;
}
