#define _GNU_SOURCE
#include "bench_backend.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct posix_backend {
  char *root;
  int root_fd;
};

struct posix_file {
  int fd;
};

static int posix_path(const struct posix_backend *state, const char *path,
                      char *output, size_t capacity) {
  const char *relative = path;
  int length;

  while (*relative == '/')
    relative++;
  if (*relative == '\0')
    length = snprintf(output, capacity, "%s", state->root);
  else
    length = snprintf(output, capacity, "%s/%s", state->root, relative);
  if (length < 0 || (size_t)length >= capacity)
    return -ENAMETOOLONG;
  return 0;
}

static int posix_open_backend(struct bench_backend *backend,
                              const char *target) {
  struct posix_backend *state;
  char *resolved;

  resolved = realpath(target, NULL);
  if (!resolved)
    return -errno;
  state = calloc(1, sizeof(*state));
  if (!state) {
    free(resolved);
    return -ENOMEM;
  }
  state->root = resolved;
  state->root_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (state->root_fd < 0) {
    int error = -errno;
    free(state->root);
    free(state);
    return error;
  }
  backend->private_data = state;
  return 0;
}

static void posix_close_backend(struct bench_backend *backend) {
  struct posix_backend *state = backend->private_data;

  if (!state)
    return;
  if (state->root_fd >= 0)
    close(state->root_fd);
  free(state->root);
  free(state);
  backend->private_data = NULL;
}

static int posix_create_file(struct bench_backend *backend, const char *path,
                             struct bench_file *file) {
  struct posix_backend *state = backend->private_data;
  struct posix_file *opened;
  char resolved[PATH_MAX];
  int error;

  error = posix_path(state, path, resolved, sizeof(resolved));
  if (error)
    return error;
  opened = malloc(sizeof(*opened));
  if (!opened)
    return -ENOMEM;
  opened->fd =
      open(resolved, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
  if (opened->fd < 0) {
    error = -errno;
    free(opened);
    return error;
  }
  file->private_data = opened;
  return 0;
}

static int posix_open_file(struct bench_backend *backend, const char *path,
                           int writable, struct bench_file *file) {
  struct posix_backend *state = backend->private_data;
  struct posix_file *opened;
  char resolved[PATH_MAX];
  int error;

  error = posix_path(state, path, resolved, sizeof(resolved));
  if (error)
    return error;
  opened = malloc(sizeof(*opened));
  if (!opened)
    return -ENOMEM;
  opened->fd = open(resolved, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
  if (opened->fd < 0) {
    error = -errno;
    free(opened);
    return error;
  }
  file->private_data = opened;
  return 0;
}

static ssize_t posix_read_file(struct bench_backend *backend,
                               struct bench_file *file, void *buffer,
                               size_t length) {
  struct posix_file *opened = file->private_data;
  ssize_t result;

  (void)backend;
  do {
    result = read(opened->fd, buffer, length);
  } while (result < 0 && errno == EINTR);
  return result < 0 ? -errno : result;
}

static ssize_t posix_write_file(struct bench_backend *backend,
                                struct bench_file *file, const void *buffer,
                                size_t length) {
  struct posix_file *opened = file->private_data;
  ssize_t result;

  (void)backend;
  do {
    result = write(opened->fd, buffer, length);
  } while (result < 0 && errno == EINTR);
  return result < 0 ? -errno : result;
}

static int posix_seek_file(struct bench_backend *backend,
                           struct bench_file *file, uint64_t offset) {
  struct posix_file *opened = file->private_data;

  (void)backend;
  if (offset > (uint64_t)INT64_MAX)
    return -EOVERFLOW;
  if (lseek(opened->fd, (off_t)offset, SEEK_SET) < 0)
    return -errno;
  return 0;
}

static int posix_close_file(struct bench_backend *backend,
                            struct bench_file *file) {
  struct posix_file *opened = file->private_data;
  int error = 0;

  (void)backend;
  if (!opened)
    return 0;
  if (close(opened->fd) != 0)
    error = -errno;
  free(opened);
  file->private_data = NULL;
  return error;
}

static int posix_mkdir(struct bench_backend *backend, const char *path) {
  struct posix_backend *state = backend->private_data;
  char resolved[PATH_MAX];
  int error = posix_path(state, path, resolved, sizeof(resolved));

  if (error)
    return error;
  if (mkdir(resolved, 0755) != 0)
    return -errno;
  return 0;
}

static int posix_stat(struct bench_backend *backend, const char *path,
                      struct bench_stat *out) {
  struct posix_backend *state = backend->private_data;
  struct stat status;
  char resolved[PATH_MAX];
  int error = posix_path(state, path, resolved, sizeof(resolved));

  if (error)
    return error;
  if (stat(resolved, &status) != 0)
    return -errno;
  out->size = (uint64_t)status.st_size;
  out->mode = (uint32_t)status.st_mode;
  return 0;
}

static int posix_iterate(struct bench_backend *backend, const char *path,
                         bench_dir_callback callback, void *opaque) {
  struct posix_backend *state = backend->private_data;
  char resolved[PATH_MAX];
  DIR *directory;
  struct dirent *entry;
  int error = posix_path(state, path, resolved, sizeof(resolved));

  if (error)
    return error;
  directory = opendir(resolved);
  if (!directory)
    return -errno;
  errno = 0;
  while ((entry = readdir(directory)) != NULL) {
    error = callback(entry->d_name, opaque);
    if (error)
      break;
    errno = 0;
  }
  if (!error && errno)
    error = -errno;
  if (closedir(directory) != 0 && !error)
    error = -errno;
  return error;
}

static int posix_rename(struct bench_backend *backend, const char *old_path,
                        const char *new_path) {
  struct posix_backend *state = backend->private_data;
  char old_resolved[PATH_MAX];
  char new_resolved[PATH_MAX];
  int error;

  error = posix_path(state, old_path, old_resolved, sizeof(old_resolved));
  if (error)
    return error;
  error = posix_path(state, new_path, new_resolved, sizeof(new_resolved));
  if (error)
    return error;
  if (rename(old_resolved, new_resolved) != 0)
    return -errno;
  return 0;
}

static int posix_unlink(struct bench_backend *backend, const char *path) {
  struct posix_backend *state = backend->private_data;
  char resolved[PATH_MAX];
  int error = posix_path(state, path, resolved, sizeof(resolved));

  if (error)
    return error;
  if (unlink(resolved) != 0)
    return -errno;
  return 0;
}

static int posix_rmdir(struct bench_backend *backend, const char *path) {
  struct posix_backend *state = backend->private_data;
  char resolved[PATH_MAX];
  int error = posix_path(state, path, resolved, sizeof(resolved));

  if (error)
    return error;
  if (rmdir(resolved) != 0)
    return -errno;
  return 0;
}

static int posix_flush(struct bench_backend *backend) {
  struct posix_backend *state = backend->private_data;

  if (syncfs(state->root_fd) != 0)
    return -errno;
  return 0;
}

static int posix_space(struct bench_backend *backend, struct bench_space *out) {
  struct posix_backend *state = backend->private_data;
  struct statvfs status;

  if (fstatvfs(state->root_fd, &status) != 0)
    return -errno;
  out->total_blocks = status.f_blocks;
  out->free_blocks = status.f_bfree;
  out->block_size = status.f_frsize ? (uint32_t)status.f_frsize
                                    : (uint32_t)status.f_bsize;
  return 0;
}

static const struct bench_backend_ops posix_ops = {
    .open = posix_open_backend,
    .close = posix_close_backend,
    .create_file = posix_create_file,
    .open_file = posix_open_file,
    .read_file = posix_read_file,
    .write_file = posix_write_file,
    .seek_file = posix_seek_file,
    .close_file = posix_close_file,
    .mkdir = posix_mkdir,
    .stat = posix_stat,
    .iterate = posix_iterate,
    .rename = posix_rename,
    .unlink = posix_unlink,
    .rmdir = posix_rmdir,
    .flush = posix_flush,
    .space = posix_space,
};

int bench_backend_create(struct bench_backend *backend) {
  if (!backend)
    return -EINVAL;
  memset(backend, 0, sizeof(*backend));
  backend->name = "posix";
  backend->ops = &posix_ops;
  return 0;
}

void bench_backend_destroy(struct bench_backend *backend) {
  if (backend && backend->private_data)
    backend->ops->close(backend);
}
