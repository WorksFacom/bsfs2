/**
 * @file restore.c
 * @brief Implements the restore command for the BSFS filesystem.
 *
 * Restores a deleted file from the Recovery tree by its recovery ID.
 *
 * Usage:
 *  - restore &lt;recovery_id&gt;
 *  - restore &lt;recovery_id&gt; &lt;dest_dir&gt;
 */

#include "../../include/api_bsfs.h"

/* See header (commands_bsfs.h) for command documentation. */
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
 * @brief Restore a deleted file by recovery ID or original name.
 *
 * If the first argument parses as a numeric ID, restores that snapshot.
 * Otherwise treats it as a name and disambiguates if multiple snapshots share
 * the same name, printing a table of candidates with deletion timestamps.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args argv-like vector; id or name in args[1], optional dest dir in
 * args[2].
 * @return 1 to continue the browser loop.
 */
int restore(FILE *disk, bsfs_super_block *sb, char **args) {
  if (!args[1]) {
    printf("Usage: restore <recovery_id|name> [dest_dir]\n");
    return 1;
  }

  char *end = NULL;
  unsigned long val = strtoul(args[1], &end, 10);
  if (!(end == args[1] || *end != '\0') && val != 0 && val <= 0xFFFFFFFFUL) {
    // ID path
    const char *dest = NULL;
    if (args[2] && args[2][0] != '\0')
      dest = args[2];
    if (restore_bsfs(disk, sb, (uint32_t)val, dest) != 0) {
      if (errno == EACCES)
        printf("Permission denied\n");
      else
        printf("Error restoring recovery entry: %s\n", args[1]);
      return 1;
    }
    if (dest)
      printf("Restored recovery entry %s into '%s'\n", args[1], dest);
    else
      printf("Restored recovery entry %s\n", args[1]);
    return 1;
  }

  // Name path
  const char *name = args[1];
  const char *dest = NULL;
  if (args[2] && args[2][0] != '\0')
    dest = args[2];

  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h)) {
    printf("Recovery tree not available.\n");
    return 1;
  }

  rec_entry_t matches[64];
  size_t total = 0;
  if (!rec_tree_find_by_name(disk, &rec_h, name, matches,
                             sizeof(matches) / sizeof(matches[0]), &total)) {
    printf("Error searching recovery entries.\n");
    return 1;
  }
  if (total == 0) {
    printf("No recovery entries named '%s'.\n", name);
    return 1;
  }
  if (total == 1) {
    // Unique match: restore by its ID
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%u", matches[0].recovery_id);
    if (restore_bsfs(disk, sb, matches[0].recovery_id, dest) != 0) {
      if (errno == EACCES)
        printf("Permission denied\n");
      else
        printf("Error restoring '%s' (id %s).\n", name, idbuf);
      return 1;
    }
    if (dest)
      printf("Restored '%s' (id %s) into '%s'\n", name, idbuf, dest);
    else
      printf("Restored '%s' (id %s)\n", name, idbuf);
    return 1;
  }

  // Multiple matches: show choices with deletion timestamps for disambiguation
  printf("Multiple deleted entries named '%s':\n", name);
  printf("%-6s %-19s %-8s %-10s %s\n", "ID", "Deleted At", "Size", "ParentIno",
         "Name");
  printf("%-6s %-19s %-8s %-10s %s\n", "--", "-------------------", "----",
         "----------", "----");

  for (size_t i = 0; i < total && i < (sizeof(matches) / sizeof(matches[0]));
       i++) {
    char delbuf[32];
    fmt_time(matches[i].deleted_at, delbuf, sizeof delbuf);
    printf("%-6u %-19s %-8u %-10u %s\n", matches[i].recovery_id, delbuf,
           matches[i].file_size, matches[i].original_parent_ino,
           matches[i].original_name);
  }
  printf("Hint: run 'restore <id>%s' to select one of the above.\n",
         dest ? " [dest_dir]" : "");
  return 1;
}
