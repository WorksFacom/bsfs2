/**
 * @file rm.c
 * @brief Implements the rm command for the BSFS filesystem.
 *
 * Removes files from the filesystem.
 */
#include "../../include/api_bsfs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/**
 * @brief Obtain a monotonic timestamp in seconds.
 */
static double monotonic_seconds(void) {
#ifdef CLOCK_MONOTONIC
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#else
  return (double)clock() / CLOCKS_PER_SEC;
#endif
}

/**
 * @brief Join a directory path and entry name into out buffer.
 */
static int join_child_path(const char *dir_path, const char *name, char *out,
                           size_t cap) {
  if (!dir_path || !name || !out || cap == 0)
    return -1;

  size_t len = 0;
  if (strcmp(dir_path, "/") == 0) {
    len = (size_t)snprintf(out, cap, "/%s", name);
  } else if (dir_path[0] == '\0') {
    len = (size_t)snprintf(out, cap, "%s", name);
  } else if (dir_path[strlen(dir_path) - 1] == '/') {
    len = (size_t)snprintf(out, cap, "%s%s", dir_path, name);
  } else {
    len = (size_t)snprintf(out, cap, "%s/%s", dir_path, name);
  }

  if (len >= cap)
    return -1;
  return 0;
}

/**
 * @brief Remove all regular files within a directory and report timing.
 */
static int rm_bulk(FILE *disk, superblock_t *sb, const char *dir_path) {
  if (!dir_path) {
    printf("rm: missing directory path for --all option\n");
    return 1;
  }

  dir_t dir;
  if (opendir_bsfs(disk, sb, dir_path, &dir) != 0) {
    if (errno == EACCES)
      printf("rm: permission denied opening '%s'\n", dir_path);
    else
      printf("rm: cannot open directory '%s'\n", dir_path);
    return 1;
  }

  dir_entry_t entry;
  uint32_t removed = 0;
  uint32_t failed = 0;
  uint32_t skipped = 0;
  char target[PATH_MAX];

  double start = monotonic_seconds();

  while (readdir_bsfs(disk, sb, &dir, &entry) == 1) {
    if (strcmp(entry.filename, ".") == 0 || strcmp(entry.filename, "..") == 0)
      continue;

    if (entry.file_type != FILE_TYPE_FILE) {
      skipped++;
      continue;
    }

    if (join_child_path(dir_path, entry.filename, target, sizeof(target)) != 0) {
      printf("rm: path too long: %s/%s\n", dir_path, entry.filename);
      failed++;
      continue;
    }

    if (remove_file_bsfs(disk, sb, target) != 0) {
      if (errno == EACCES)
        printf("rm: permission denied removing %s\n", target);
      else
        printf("rm: failed removing %s (errno=%d)\n", target, errno);
      failed++;
      continue;
    }

    removed++;
  }

  closedir_bsfs(&dir);

  double elapsed_ms = (monotonic_seconds() - start) * 1000.0;

  printf("rm: removed %u files from %s in %.6f ms (failures=%u, skipped=%u)\n",
         removed, dir_path, elapsed_ms, failed, skipped);

  return 1;
}

/**
 * @brief Remove a file (logical delete with Recovery snapshot).
 *
 * Unlinks the directory entry and inserts a snapshot into the Recovery tree
 * so the file can be restored later. Prints a confirmation or a concise
 * error message (including permission denied when applicable).
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Command argv‑like vector; expects target path in args[1].
 * @return 1 to continue the browser loop.
 */
int rm(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: rm <filename>\n");
    printf("       rm --all [directory]\n");
    return 1;
  }

  if (strcmp(args[1], "--all") == 0 || strcmp(args[1], "-a") == 0) {
    const char *dir_path = args[2] ? args[2] : ".";
    return rm_bulk(disk, sb, dir_path);
  }

  if (remove_file_bsfs(disk, sb, args[1]) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error removing file: %s (errno=%d)\n", args[1], errno);
    return 1;
  }

  printf("File removed: %s\n", args[1]);
  return 1;
}
