#define _GNU_SOURCE
#include "bench_backend.h"

#include "api_bsfs.h"
#include "bitmap_operations_bsfs.h"
#include "btree_bsfs.h"
#include "rec_gc_bsfs.h"
#include "rec_entry_operations_bsfs.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct old_backend {
  FILE *device;
  superblock_t super;
};

struct old_file {
  file_t file;
};

struct recovery_collection {
  struct bench_recovery_entry *entries;
  size_t capacity;
  size_t count;
  int error;
};

static int old_error(int fallback) {
  return errno ? -errno : -fallback;
}

static int old_open_backend(struct bench_backend *backend,
                            const char *target) {
  struct old_backend *state = calloc(1, sizeof(*state));

  if (!state)
    return -ENOMEM;
  state->device = fopen(target, "r+b");
  if (!state->device) {
    int error = -errno;
    free(state);
    return error;
  }
  if (read_superblock(state->device, &state->super) != 0) {
    int error = old_error(EINVAL);
    fclose(state->device);
    free(state);
    return error;
  }
  backend->private_data = state;
  return 0;
}

static void old_close_backend(struct bench_backend *backend) {
  struct old_backend *state = backend->private_data;

  if (!state)
    return;
  if (state->device)
    fclose(state->device);
  free(state);
  backend->private_data = NULL;
}

static int old_create_file(struct bench_backend *backend, const char *path,
                           struct bench_file *file) {
  struct old_backend *state = backend->private_data;
  struct old_file *opened;

  errno = 0;
  if (create_file_bsfs(state->device, &state->super, path) != 0)
    return old_error(EIO);
  opened = calloc(1, sizeof(*opened));
  if (!opened)
    return -ENOMEM;
  errno = 0;
  if (open_bsfs(state->device, &state->super, path, &opened->file) != 0) {
    int error = old_error(EIO);
    free(opened);
    return error;
  }
  file->private_data = opened;
  return 0;
}

static int old_open_file(struct bench_backend *backend, const char *path,
                         int writable, struct bench_file *file) {
  struct old_backend *state = backend->private_data;
  struct old_file *opened = calloc(1, sizeof(*opened));

  (void)writable;
  if (!opened)
    return -ENOMEM;
  errno = 0;
  if (open_bsfs(state->device, &state->super, path, &opened->file) != 0) {
    int error = old_error(ENOENT);
    free(opened);
    return error;
  }
  file->private_data = opened;
  return 0;
}

static ssize_t old_read_file(struct bench_backend *backend,
                             struct bench_file *file, void *buffer,
                             size_t length) {
  struct old_backend *state = backend->private_data;
  struct old_file *opened = file->private_data;
  size_t received;

  if (length > UINT32_MAX)
    return -EOVERFLOW;
  errno = 0;
  received = read_bsfs(state->device, &state->super, &opened->file, buffer,
                       (uint32_t)length);
  if (received == 0 && errno)
    return -errno;
  return (ssize_t)received;
}

static ssize_t old_write_file(struct bench_backend *backend,
                              struct bench_file *file, const void *buffer,
                              size_t length) {
  struct old_backend *state = backend->private_data;
  struct old_file *opened = file->private_data;
  size_t written;

  if (length > UINT32_MAX)
    return -EOVERFLOW;
  errno = 0;
  written = write_bsfs(state->device, &state->super, &opened->file,
                       (void *)buffer, (uint32_t)length);
  if (written == 0)
    return old_error(ENOSPC);
  return (ssize_t)written;
}

static int old_seek_file(struct bench_backend *backend,
                         struct bench_file *file, uint64_t offset) {
  struct old_file *opened = file->private_data;

  (void)backend;
  if (offset > UINT32_MAX)
    return -EOVERFLOW;
  errno = 0;
  if (seek_bsfs(&opened->file, (uint32_t)offset) != 0)
    return old_error(EINVAL);
  return 0;
}

static int old_close_file(struct bench_backend *backend,
                          struct bench_file *file) {
  struct old_file *opened = file->private_data;
  int result;

  (void)backend;
  if (!opened)
    return 0;
  result = close_bsfs(&opened->file);
  free(opened);
  file->private_data = NULL;
  return result == 0 ? 0 : old_error(EIO);
}

static int old_mkdir(struct bench_backend *backend, const char *path) {
  struct old_backend *state = backend->private_data;

  errno = 0;
  if (mkdir_bsfs(state->device, &state->super, path) != 0)
    return old_error(EIO);
  return 0;
}

static int old_stat(struct bench_backend *backend, const char *path,
                    struct bench_stat *out) {
  struct old_backend *state = backend->private_data;
  inode_t inode;

  errno = 0;
  if (stat_bsfs(state->device, &state->super, path, &inode) != 0)
    return old_error(ENOENT);
  out->size = inode.file_size;
  out->mode = (inode.file_type == FILE_TYPE_DIR ? S_IFDIR : S_IFREG) |
              inode.permissions;
  return 0;
}

static int old_iterate(struct bench_backend *backend, const char *path,
                       bench_dir_callback callback, void *opaque) {
  struct old_backend *state = backend->private_data;
  dir_t directory;
  dir_entry_t entry;
  int result;

  errno = 0;
  if (opendir_bsfs(state->device, &state->super, path, &directory) != 0)
    return old_error(ENOENT);
  while ((result = readdir_bsfs(state->device, &state->super, &directory,
                                &entry)) > 0) {
    int error = callback(entry.filename, opaque);
    if (error) {
      (void)closedir_bsfs(&directory);
      return error;
    }
  }
  (void)closedir_bsfs(&directory);
  return result < 0 ? old_error(EIO) : 0;
}

static int old_rename(struct bench_backend *backend, const char *old_path,
                      const char *new_path) {
  struct old_backend *state = backend->private_data;

  errno = 0;
  if (move_bsfs(state->device, &state->super, old_path, new_path) != 0)
    return old_error(EIO);
  return 0;
}

static int old_unlink(struct bench_backend *backend, const char *path) {
  struct old_backend *state = backend->private_data;

  errno = 0;
  if (remove_file_bsfs(state->device, &state->super, path) != 0)
    return old_error(EIO);
  return 0;
}

static int old_rmdir(struct bench_backend *backend, const char *path) {
  struct old_backend *state = backend->private_data;

  errno = 0;
  if (rmdir_bsfs(state->device, &state->super, path) != 0)
    return old_error(EIO);
  return 0;
}

static int old_flush(struct bench_backend *backend) {
  struct old_backend *state = backend->private_data;

  if (fflush(state->device) != 0)
    return -errno;
  if (fdatasync(fileno(state->device)) != 0)
    return -errno;
  return 0;
}

static int old_space(struct bench_backend *backend, struct bench_space *out) {
  struct old_backend *state = backend->private_data;
  uint8_t *bitmap;
  uint64_t free_blocks = 0;

  bitmap = load_bitmap(state->device, state->super.block_bitmap_start);
  if (!bitmap)
    return -EIO;
  for (uint32_t block = state->super.data_block_start;
       block < state->super.total_blocks; block++) {
    if (is_bit_free(bitmap, block))
      free_blocks++;
  }
  free(bitmap);
  out->total_blocks =
      state->super.total_blocks - state->super.data_block_start;
  out->free_blocks = free_blocks;
  out->block_size = state->super.block_size;
  return 0;
}

static void collect_recovery_node(struct old_backend *state,
                                  btree_handle_t *tree, uint32_t block,
                                  struct recovery_collection *collection) {
  btree_node_t *node;

  if (!block || collection->error)
    return;
  node = btree_read_node(state->device, tree, block);
  if (!node) {
    collection->error = -EIO;
    return;
  }
  for (uint32_t i = 0; i < node->num_entries; i++) {
    rec_entry_t *entry;

    if (!node->is_leaf)
      collect_recovery_node(state, tree, node->children[i], collection);
    if (collection->error)
      break;
    entry = (rec_entry_t *)((unsigned char *)node->entries +
                            (size_t)i * tree->entry_size);
    if (collection->count < collection->capacity) {
      struct bench_recovery_entry *out = &collection->entries[collection->count];

      out->id = entry->recovery_id;
      snprintf(out->name, sizeof(out->name), "%s", entry->original_name);
    }
    collection->count++;
  }
  if (!collection->error && !node->is_leaf)
    collect_recovery_node(state, tree, node->children[node->num_entries],
                          collection);
  btree_free_node(node);
}

static int old_recovery_list(struct bench_backend *backend,
                             struct bench_recovery_entry *entries,
                             size_t capacity, size_t *count) {
  struct old_backend *state = backend->private_data;
  struct recovery_collection collection = {
      .entries = entries,
      .capacity = capacity,
  };
  btree_handle_t *tree;

  if (!count)
    return -EINVAL;
  *count = 0;
  if (state->super.recovery_root == 0)
    return 0;
  tree = btree_create_rec_entry_handle();
  if (!tree)
    return -ENOMEM;
  tree->root_block = state->super.recovery_root;
  collect_recovery_node(state, tree, tree->root_block, &collection);
  free(tree);
  *count = collection.count;
  if (collection.error)
    return collection.error;
  if (collection.count > capacity)
    return -ENOSPC;
  return 0;
}

static int old_recovery_restore(struct bench_backend *backend, uint32_t id,
                                const char *destination) {
  struct old_backend *state = backend->private_data;

  errno = 0;
  if (restore_bsfs(state->device, &state->super, id, destination) != 0)
    return old_error(EIO);
  return 0;
}

static int old_recovery_purge(struct bench_backend *backend, uint64_t blocks,
                              uint64_t *blocks_reclaimed) {
  struct old_backend *state = backend->private_data;
  uint64_t total = 0;

  if (blocks == 0)
    return -EINVAL;
  while (total < blocks) {
    uint32_t request =
        blocks - total > UINT32_MAX ? UINT32_MAX : (uint32_t)(blocks - total);
    size_t reclaimed =
        rec_gc_purge_until(state->device, &state->super, request, UINT64_MAX);
    if (reclaimed == 0)
      break;
    total += reclaimed;
  }
  if (blocks_reclaimed)
    *blocks_reclaimed = total;
  return total ? 0 : -ENOSPC;
}

static const struct bench_backend_ops old_ops = {
    .open = old_open_backend,
    .close = old_close_backend,
    .create_file = old_create_file,
    .open_file = old_open_file,
    .read_file = old_read_file,
    .write_file = old_write_file,
    .seek_file = old_seek_file,
    .close_file = old_close_file,
    .mkdir = old_mkdir,
    .stat = old_stat,
    .iterate = old_iterate,
    .rename = old_rename,
    .unlink = old_unlink,
    .rmdir = old_rmdir,
    .flush = old_flush,
    .space = old_space,
    .recovery_list = old_recovery_list,
    .recovery_restore = old_recovery_restore,
    .recovery_purge = old_recovery_purge,
};

int bench_backend_create(struct bench_backend *backend) {
  if (!backend)
    return -EINVAL;
  memset(backend, 0, sizeof(*backend));
  backend->name = "old-bsfs";
  backend->ops = &old_ops;
  return 0;
}

void bench_backend_destroy(struct bench_backend *backend) {
  if (backend && backend->private_data)
    backend->ops->close(backend);
}
