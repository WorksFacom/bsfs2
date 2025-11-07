/**
 * @file rm.c
 * @brief Implements the rm command for the BSFS filesystem.
 *
 * Removes files from the filesystem.
 */
#include "../../include/api_bsfs.h"
#include "../../include/dir_entry_operations_bsfs.h"
#include "../../include/inode_operations_bsfs.h"
#include "../../include/rec_entry_operations_bsfs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
/** Maximum path length handled by rm (fallback when system PATH_MAX is absent). */
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
 * @struct rm_task_t
 * @brief Prepared removal record used by the bulk rm implementation.
 *
 * Each task captures the absolute path and the inode snapshot for a file that
 * will be deleted, along with metadata needed to create a Recovery entry if the
 * removal succeeds.
 */
typedef struct {
  char *path;             /**< Absolute path of the file scheduled for removal. */
  inode_t inode_snapshot; /**< Inode contents captured before unlinking. */
  uint32_t parent_ino;    /**< Inode number of the parent directory. */
  int has_snapshot; /**< Flag indicating whether inode_snapshot is populated. */
} rm_task_t;

/**
 * @brief Remove a file while preserving a Recovery snapshot.
 *
 * Opens the parent directory, records the file's metadata into the Recovery
 * tree, removes the directory entry, and finally deletes the inode from the
 * inode B-tree. On failure, the recovery entry is rolled back.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param task Prepared removal descriptor with full path and inode snapshot.
 * @return 0 on success; -1 on error (no changes committed).
 */
static int remove_with_snapshot(FILE *disk, superblock_t *sb,
                                const rm_task_t *task) {
  if (!task || !task->has_snapshot)
    return -1;

  char parent_path[PATH_MAX];
  const char *child = task->path;
  const char *slash = strrchr(task->path, '/');
  if (slash) {
    size_t parent_len = (size_t)(slash - task->path);
    if (parent_len >= sizeof(parent_path))
      return -1;
    memcpy(parent_path, task->path, parent_len);
    parent_path[parent_len] = '\0';
    child = slash + 1;
  } else {
    parent_path[0] = '.';
    parent_path[1] = '\0';
  }

  dir_t cwd_snapshot;
  if (opendir_bsfs(disk, sb, parent_path, &cwd_snapshot) != 0)
    return -1;

  btree_handle_t *dir_h = btree_create_dir_entry_handle();
  if (!dir_h) {
    closedir_bsfs(&cwd_snapshot);
    return -1;
  }
  dir_h->root_block = cwd_snapshot.inode.btree_root;

  dir_entry_t entry;
  if (!scan_dir_tree_find(disk, dir_h, dir_h->root_block, child, &entry)) {
    free(dir_h);
    closedir_bsfs(&cwd_snapshot);
    return -1;
  }
  free(dir_h);

  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h)) {
    closedir_bsfs(&cwd_snapshot);
    return -1;
  }

  rec_entry_t rec;
  memset(&rec, 0, sizeof(rec));
  rec.deleted_at = (uint64_t)time(NULL);
  rec.original_parent_ino = task->parent_ino;
  strncpy(rec.original_name, child, sizeof(rec.original_name) - 1);
  rec.original_name[sizeof(rec.original_name) - 1] = '\0';
  rec.inode_number = task->inode_snapshot.inode_number;
  rec.file_type = task->inode_snapshot.file_type;
  rec.uid = task->inode_snapshot.uid;
  rec.gid = task->inode_snapshot.gid;
  rec.permissions = task->inode_snapshot.permissions;
  rec.file_size = task->inode_snapshot.file_size;
  rec.block_count = task->inode_snapshot.block_count;
  memcpy(rec.direct_blocks, task->inode_snapshot.direct_blocks,
         sizeof(rec.direct_blocks));
  rec.btree_root = task->inode_snapshot.btree_root;
  rec.retention_until = rec.deleted_at + DEFAULT_RECOVERY_TTL;

  if (!rec_tree_add(disk, sb, &rec_h, &rec)) {
    closedir_bsfs(&cwd_snapshot);
    return -1;
  }

  if (remove_dir_entry(disk, sb, &cwd_snapshot, child) != 0) {
    (void)rec_tree_remove(disk, sb, &rec_h, rec.recovery_id);
    closedir_bsfs(&cwd_snapshot);
    return -1;
  }

  btree_handle_t *inode_h = btree_create_inode_handle();
  if (inode_h) {
    inode_h->root_block = sb->inode_root;
    (void)remove_inode(disk, sb, inode_h, task->inode_snapshot.inode_number);
    free(inode_h);
  }

  closedir_bsfs(&cwd_snapshot);
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

  rm_task_t *tasks = NULL;
  size_t allocated = 0;
  size_t count = 0;
  uint32_t skipped = 0;
  uint32_t prep_failed = 0;
  dir_entry_t entry;

  btree_handle_t *inode_enum = btree_create_inode_handle();
  if (inode_enum)
    inode_enum->root_block = sb->inode_root;

  while (readdir_bsfs(disk, sb, &dir, &entry) == 1) {
    if (strcmp(entry.filename, ".") == 0 || strcmp(entry.filename, "..") == 0)
      continue;

    if (entry.file_type != FILE_TYPE_FILE) {
      skipped++;
      continue;
    }

    char buffer[PATH_MAX];
    if (join_child_path(dir_path, entry.filename, buffer, sizeof(buffer)) != 0) {
      printf("rm: path too long: %s/%s\n", dir_path, entry.filename);
      prep_failed++;
      continue;
    }

    if (count == allocated) {
      size_t new_cap = allocated ? allocated * 2 : 32;
      rm_task_t *tmp = realloc(tasks, new_cap * sizeof(rm_task_t));
      if (!tmp) {
        printf("rm: out of memory while enumerating '%s'\n", dir_path);
        skipped++; /* treat remaining entries as skipped */
        break;
      }
      tasks = tmp;
      allocated = new_cap;
    }

    const char *store_path = buffer;
    if (store_path[0] == '.' && store_path[1] == '/' && store_path[2] != '\0')
      store_path += 2;

    memset(&tasks[count], 0, sizeof(rm_task_t));
    tasks[count].path = strdup(store_path);
    if (!tasks[count].path) {
      printf("rm: out of memory while storing '%s'\n", buffer);
      skipped++;
      prep_failed++;
      continue;
    }
    tasks[count].parent_ino = dir.inode.inode_number;
    if (inode_enum &&
        read_inode(disk, inode_enum, entry.inode_number,
                   &tasks[count].inode_snapshot)) {
      tasks[count].has_snapshot = 1;
    }
    count++;
  }

  closedir_bsfs(&dir);
  if (inode_enum)
    free(inode_enum);

  uint32_t removed = 0;
  uint32_t failed = 0;

  double start = monotonic_seconds();

  for (size_t i = 0; i < count; i++) {
    rm_task_t *task = &tasks[i];
    int status = -1;
    if (task->has_snapshot)
      status = remove_with_snapshot(disk, sb, task);

    if (status != 0) {
      if (remove_file_bsfs(disk, sb, task->path) != 0) {
        if (errno == EACCES)
          printf("rm: permission denied removing %s\n", task->path);
        else
          printf("rm: failed removing %s (errno=%d)\n", task->path, errno);
        failed++;
      } else {
        removed++;
      }
    } else {
      removed++;
    }
    free(task->path);
  }
  free(tasks);

  double elapsed_ms = (monotonic_seconds() - start) * 1000.0;

  failed += prep_failed;

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
 * @param args Command argv-like vector; expects target path in args[1].
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
