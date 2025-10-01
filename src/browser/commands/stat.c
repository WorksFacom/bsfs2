/**
 * @file stat.c
 * @brief Implements the stat command for the BSFS filesystem.
 *
 * Shows metadata information about a file or directory.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Convert an inode file type code to a readable string.
 *
 * Maps BSFS file type codes to human-friendly labels used by the
 * browser UI.
 *
 * @param t File type code (e.g., FILE_TYPE_DIR or FILE_TYPE_FILE).
 * @return "DIR" for directories; "FILE" otherwise.
 */
static const char *type_str(uint8_t t) {
  return (t == FILE_TYPE_DIR) ? "DIR" : "FILE";
}

/**
 * @brief Print a human-readable timestamp with a label.
 *
 * Formats the given POSIX timestamp as "YYYY-mm-dd HH:MM:SS" and prints it
 * prefixed by the provided label. Falls back to printing the raw integer if
 * conversion fails.
 *
 * @param label Descriptive label to print before the timestamp.
 * @param t     POSIX time value (seconds since epoch).
 */
static void print_time(const char *label, uint32_t t) {
  time_t tt = (time_t)t;
  struct tm *tm = localtime(&tt);
  if (tm) {
    char buf[64];
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm) > 0) {
      printf("%s: %s\n", label, buf);
      return;
    }
  }
  printf("%s: %u\n", label, t);
}

/**
 * @brief Show inode metadata for a path.
 *
 * Usage: stat &lt;path&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Path in args[1].
 * @return 1 to continue the REPL.
 */
int stat(FILE *disk, superblock_t *sb, char **args) {
  const char *path = args[1];
  if (!path) {
    printf("Usage: stat <path>\n");
    return 1;
  }

  inode_t inode;
  if (stat_bsfs(disk, sb, path, &inode) != 0) {
    printf("stat: cannot stat '%s' (not found)\n", path);
    return 1;
  }

  printf("Path: %s\n", path);
  printf("Type: %s\n", type_str(inode.file_type));
  printf("Inode: %u\n", inode.inode_number);
  printf("Size: %u bytes\n", inode.file_size);
  printf("Blocks: %u\n", inode.block_count);
  printf("UID: %u  GID: %u\n", (unsigned)inode.uid, (unsigned)inode.gid);
  printf("Permissions: 0%o\n", (unsigned)inode.permissions);
  print_time("Created", inode.created_at);
  print_time("Modified", inode.modified_at);
  print_time("Accessed", inode.accessed_at);

  return 1;
}
