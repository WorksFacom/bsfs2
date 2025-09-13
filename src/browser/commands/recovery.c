/**
 * @file recovery.c
 * @brief Implements the recovery listing command for the BSFS filesystem.
 *
 * Usage:
 *  - recovery           -> list all recovery entries
 *  - recovery <name>    -> list entries filtered by original filename
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Format a POSIX timestamp into a fixed-size buffer.
 *
 * Writes a human-readable string "YYYY-mm-dd HH:MM:SS" to @p buf when @p ts
 * is non-zero. When @p ts is 0, writes a single dash ("-"). Uses
 * localtime_r for thread-safe conversion.
 *
 * @param ts  POSIX time (seconds since epoch).
 * @param buf Output buffer to receive the formatted string.
 * @param cap Capacity of @p buf in bytes.
 */
static void fmt_time(uint64_t ts, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  if (ts == 0) {
    snprintf(buf, cap, "-");
    return;
  }
  time_t t = (time_t)ts;
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

/**
 * @brief Traverse the Recovery B-tree in order and print matching entries.
 *
 * Performs an in-order traversal starting at @p node_block, printing a table
 * row for each recovery entry. If @p filter is non-NULL, only entries whose
 * original_name matches exactly are printed. Increments @p printed for every
 * row emitted.
 *
 * @param disk       Open filesystem image.
 * @param handle     Recovery tree handle (configured for rec_entry_t).
 * @param node_block Current node block to visit.
 * @param filter     Optional exact name filter (NULL to print all entries).
 * @param printed    In/out: counter of printed rows.
 * @return 1 on success; 0 if node read/traversal fails.
 */
static int list_recovery_inorder(FILE *disk, btree_handle_t *handle,
                                 uint32_t node_block, const char *filter,
                                 size_t *printed) {
  if (node_block == 0)
    return 1;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  rec_entry_t *entries = (rec_entry_t *)node->entries;
  int ok = 1;

  for (uint32_t i = 0; i < node->num_entries && ok; i++) {
    if (!node->is_leaf) {
      ok = list_recovery_inorder(disk, handle, node->children[i], filter,
                                 printed);
    }
    if (!ok)
      break;

    if (!filter || strcmp(entries[i].original_name, filter) == 0) {
      char delbuf[32], expbuf[32];
      fmt_time(entries[i].deleted_at, delbuf, sizeof(delbuf));
      fmt_time(entries[i].retention_until, expbuf, sizeof(expbuf));
      const char *type =
          (entries[i].file_type == FILE_TYPE_DIR) ? "DIR" : "FILE";
      printf("%-6u %-5s %-8u %-19s %-19s %-10u %s\n", entries[i].recovery_id,
             type, entries[i].file_size, delbuf, expbuf,
             entries[i].original_parent_ino, entries[i].original_name);
      (*printed)++;
    }
  }

  if (ok && !node->is_leaf) {
    ok = list_recovery_inorder(disk, handle, node->children[node->num_entries],
                               filter, printed);
  }

  btree_free_node(node);
  return ok;
}

/**
 * @brief BSFS browser command: list deleted file snapshots (Recovery).
 *
 * Usage:
 *  - recovery            -> list all snapshots
 *  - recovery <name>     -> list snapshots filtered by original filename
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Tokenized arguments; optional filter name in args[1].
 * @return Always 1 to continue the browser loop.
 */
int recovery(FILE *disk, superblock_t *sb, char **args) {
  (void)args;
  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h)) {
    printf("Recovery tree not available.\n");
    return 1;
  }

  if (rec_h.root_block == 0) {
    printf("No recovery entries.\n");
    return 1;
  }

  const char *filter_name = NULL;
  if (args[1] && args[1][0] != '\0') {
    filter_name = args[1];
  }

  printf("Recovery entries%s:\n", filter_name ? " (filtered)" : "");
  printf("%-6s %-5s %-8s %-19s %-19s %-10s %s\n", "ID", "Type", "Size",
         "Deleted At", "Retain Until", "ParentIno", "Name");
  printf("%-6s %-5s %-8s %-19s %-19s %-10s %s\n", "--", "----", "----",
         "-------------------", "-------------------", "----------", "----");

  size_t printed = 0;
  if (!list_recovery_inorder(disk, &rec_h, rec_h.root_block, filter_name,
                             &printed)) {
    printf("Error reading recovery entries.\n");
    return 1;
  }

  if (printed == 0) {
    if (filter_name)
      printf("No recovery entries match name '%s'.\n", filter_name);
    else
      printf("No recovery entries.\n");
  }

  return 1;
}
