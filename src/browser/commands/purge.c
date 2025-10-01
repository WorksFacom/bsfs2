/**
 * @file purge.c
 * @brief Implements the purge command for the BSFS filesystem.
 *
 * Allows users to reclaim space by removing Recovery snapshots:
 *  - purge                  -> remove entries older than ~30 days
 *  - purge &lt;id&gt;             -> remove a specific snapshot by Recovery ID
 *  - purge oldest &lt;count&gt;   -> remove the &lt;count&gt; oldest snapshots
 */
#include "../../include/api_bsfs.h"

static const uint64_t SECS_30D = 30ULL * 24ULL * 60ULL * 60ULL;

/**
 * @struct rec_item_t
 * @brief Small wrapper to carry a recovery entry during traversals/sorting.
 */
typedef struct {
  rec_entry_t entry; /**< The recovery entry copied from the B-tree node. */
} rec_item_t;

/**
 * @brief Collect all recovery entries from the tree into a linear array.
 *
 * Performs an in-order traversal starting at @p node_block, copying each
 * rec_entry_t into an expanding array buffer. The function reallocates the
 * array as needed, doubling its capacity when full.
 *
 * @param disk       Open filesystem image.
 * @param rec_h      Recovery B-tree handle.
 * @param node_block Current node block to visit.
 * @param arr        In/out: pointer to the dynamic array buffer.
 * @param count      In/out: number of valid items currently stored.
 * @param cap        In/out: current capacity of @p arr (in items).
 */
static void collect_all(FILE *disk, btree_handle_t *rec_h, uint32_t node_block,
                        rec_item_t **arr, size_t *count, size_t *cap) {
  if (node_block == 0)
    return;
  btree_node_t *node = btree_read_node(disk, rec_h, node_block);
  if (!node)
    return;
  if (!node->is_leaf) {
    for (uint32_t i = 0; i < node->num_entries; i++) {
      collect_all(disk, rec_h, node->children[i], arr, count, cap);
    }
  }
  for (uint32_t i = 0; i < node->num_entries; i++) {
    if (*count == *cap) {
      *cap = (*cap == 0) ? 64 : (*cap * 2);
      *arr = (rec_item_t *)realloc(*arr, *cap * sizeof(rec_item_t));
      if (!*arr)
        break;
    }
    memcpy(&(*arr)[*count].entry,
           (uint8_t *)node->entries + i * rec_h->entry_size,
           sizeof(rec_entry_t));
    (*count)++;
  }
  if (!node->is_leaf) {
    collect_all(disk, rec_h, node->children[node->num_entries], arr, count,
                cap);
  }
  btree_free_node(node);
}

/**
 * @brief qsort comparator for ascending deleted_at order.
 *
 * @param a Pointer to first @ref rec_item_t.
 * @param b Pointer to second @ref rec_item_t.
 * @return <0 if a<b, 0 if equal, >0 if a>b by deleted_at.
 */
static int cmp_deleted_at(const void *a, const void *b) {
  const rec_item_t *ra = (const rec_item_t *)a;
  const rec_item_t *rb = (const rec_item_t *)b;
  if (ra->entry.deleted_at < rb->entry.deleted_at)
    return -1;
  if (ra->entry.deleted_at > rb->entry.deleted_at)
    return 1;
  // Tie-break by recovery_id ascending for stable ordering
  if (ra->entry.recovery_id < rb->entry.recovery_id)
    return -1;
  if (ra->entry.recovery_id > rb->entry.recovery_id)
    return 1;
  return 0;
}

/* Internal helper; see browser command header for public docs. */
/**
 * @brief Purge a specific Recovery snapshot by its ID.
 *
 * Looks up the recovery entry with the given @p id and removes it, reclaiming
 * its data blocks (both direct spans and spans in the per-file bspan tree).
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param id   Recovery entry ID to purge.
 * @return 0 on success; -1 if the entry is not found or on error.
 */
static int purge_by_id(FILE *disk, superblock_t *sb, uint32_t id) {
  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h))
    return -1;
  rec_entry_t rec;
  if (!rec_tree_search(disk, &rec_h, id, &rec))
    return -1;
  btree_handle_t *bspan = btree_create_bspan_handle();
  bspan->root_block = rec.btree_root;
  int rc = purge_file(disk, sb, bspan, &rec_h, &rec);
  free(bspan);
  return rc;
}

/**
 * @brief Purge the N oldest Recovery snapshots.
 *
 * Traverses the Recovery tree, sorts entries by deletion time, and purges the
 * oldest @p n entries still present.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param n    Number of oldest entries to purge.
 * @return 0 on success; -1 on failure to init recovery tree.
 */
static int purge_oldest_n(FILE *disk, superblock_t *sb, size_t n) {
  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h))
    return -1;
  rec_item_t *items = NULL;
  size_t count = 0, cap = 0;
  collect_all(disk, &rec_h, rec_h.root_block, &items, &count, &cap);
  if (!items) {
    return 0;
  }
  qsort(items, count, sizeof(rec_item_t), cmp_deleted_at);
  btree_handle_t *bspan = btree_create_bspan_handle();
  size_t purged = 0;
  for (size_t i = 0; i < count && purged < n; i++) {
    // Confirm still present (may have been removed concurrently)
    rec_entry_t tmp;
    if (!rec_tree_search(disk, &rec_h, items[i].entry.recovery_id, &tmp))
      continue;
    bspan->root_block = items[i].entry.btree_root;
    if (purge_file(disk, sb, bspan, &rec_h, &items[i].entry) == 0)
      purged++;
  }
  free(bspan);
  free(items);
  printf("purge: removed %zu oldest entr%s\n", purged,
         purged == 1 ? "y" : "ies");
  return 0;
}

/**
 * @brief Purge all Recovery snapshots older than ~30 days.
 *
 * Uses the current time minus 30 days as cutoff against the deleted_at field.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @return 0 on success; -1 on failure to init recovery tree.
 */
static int purge_older_than_30d(FILE *disk, superblock_t *sb) {
  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h))
    return -1;
  uint64_t now = (uint64_t)time(NULL);
  uint64_t cutoff = (now > SECS_30D) ? (now - SECS_30D) : 0;

  rec_item_t *items = NULL;
  size_t count = 0, cap = 0;
  collect_all(disk, &rec_h, rec_h.root_block, &items, &count, &cap);
  if (!items)
    return 0;

  btree_handle_t *bspan = btree_create_bspan_handle();
  size_t purged = 0;
  for (size_t i = 0; i < count; i++) {
    if (items[i].entry.deleted_at <= cutoff) {
      // Verify still present and purge
      rec_entry_t tmp;
      if (!rec_tree_search(disk, &rec_h, items[i].entry.recovery_id, &tmp))
        continue;
      bspan->root_block = items[i].entry.btree_root;
      if (purge_file(disk, sb, bspan, &rec_h, &items[i].entry) == 0)
        purged++;
    }
  }
  free(bspan);
  free(items);
  printf("purge: removed %zu entr%s older than ~30 days\n", purged,
         purged == 1 ? "y" : "ies");
  return 0;
}

/**
 * @brief Purge all Recovery snapshots whose retention has expired.
 *
 * Removes entries where rec_entry_t::retention_until <= now.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @return 0 on success; -1 on failure to init recovery tree.
 */
static int purge_expired(FILE *disk, superblock_t *sb) {
  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h))
    return -1;
  uint64_t now = (uint64_t)time(NULL);

  rec_item_t *items = NULL;
  size_t count = 0, cap = 0;
  collect_all(disk, &rec_h, rec_h.root_block, &items, &count, &cap);
  if (!items)
    return 0;

  btree_handle_t *bspan = btree_create_bspan_handle();
  size_t purged = 0;
  for (size_t i = 0; i < count; i++) {
    if (items[i].entry.retention_until != 0 &&
        items[i].entry.retention_until <= now) {
      rec_entry_t tmp;
      if (!rec_tree_search(disk, &rec_h, items[i].entry.recovery_id, &tmp))
        continue;
      bspan->root_block = items[i].entry.btree_root;
      if (purge_file(disk, sb, bspan, &rec_h, &items[i].entry) == 0)
        purged++;
    }
  }
  free(bspan);
  free(items);
  printf("purge: removed %zu expired entr%s\n", purged,
         purged == 1 ? "y" : "ies");
  return 0;
}
/**
 * @brief BSFS browser command: purge Recovery snapshots to reclaim space.
 *
 * Usage:
 *  - purge                  -> remove entries older than ~30 days
 *  - purge &lt;id&gt;             -> remove a specific snapshot by Recovery ID
 *  - purge oldest &lt;count&gt;   -> remove the &lt;count&gt; oldest snapshots
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Tokenized arguments per usage above.
 * @return Always 1 to continue the browser loop.
 */
int purge(FILE *disk, superblock_t *sb, char **args) {
  uint16_t uid = 0, gid = 0;
  get_identity_bsfs(&uid, &gid);
  if (uid != 0) {
    printf("Permission denied\n");
    return 1;
  }
  // No args: default policy
  if (!args[1]) {
    (void)purge_older_than_30d(disk, sb);
    return 1;
  }

  // Numeric ID path
  char *end = NULL;
  unsigned long id = strtoul(args[1], &end, 10);
  if (end != args[1] && *end == '\0' && id > 0 && id <= 0xFFFFFFFFUL) {
    if (purge_by_id(disk, sb, (uint32_t)id) != 0) {
      printf("purge: recovery id %s not found or failed\n", args[1]);
      return 1;
    }
    printf("purge: removed recovery id %s\n", args[1]);
    return 1;
  }

  // Oldest N path
  if (strcmp(args[1], "oldest") == 0) {
    if (!args[2]) {
      printf("Usage: purge oldest <count>\n");
      return 1;
    }
    char *e2 = NULL;
    unsigned long n = strtoul(args[2], &e2, 10);
    if (e2 == args[2] || *e2 != '\0' || n == 0) {
      printf("purge: invalid count '%s'\n", args[2]);
      return 1;
    }
    (void)purge_oldest_n(disk, sb, (size_t)n);
    return 1;
  }

  // Expired path
  if (strcmp(args[1], "expired") == 0) {
    (void)purge_expired(disk, sb);
    return 1;
  }

  printf("Usage: purge | purge <id> | purge oldest <count>\n");
  return 1;
}
