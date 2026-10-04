#ifndef BSFS_BENCH_BACKEND_H
#define BSFS_BENCH_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct bench_file {
  void *private_data;
};

struct bench_stat {
  uint64_t size;
  uint32_t mode;
};

struct bench_space {
  uint64_t total_blocks;
  uint64_t free_blocks;
  uint32_t block_size;
};

typedef int (*bench_dir_callback)(const char *name, void *opaque);

struct bench_backend;

struct bench_backend_ops {
  int (*open)(struct bench_backend *backend, const char *target);
  void (*close)(struct bench_backend *backend);

  int (*create_file)(struct bench_backend *backend, const char *path,
                     struct bench_file *file);
  int (*open_file)(struct bench_backend *backend, const char *path,
                   int writable, struct bench_file *file);
  ssize_t (*read_file)(struct bench_backend *backend, struct bench_file *file,
                       void *buffer, size_t length);
  ssize_t (*write_file)(struct bench_backend *backend, struct bench_file *file,
                        const void *buffer, size_t length);
  int (*seek_file)(struct bench_backend *backend, struct bench_file *file,
                   uint64_t offset);
  int (*close_file)(struct bench_backend *backend, struct bench_file *file);

  int (*mkdir)(struct bench_backend *backend, const char *path);
  int (*stat)(struct bench_backend *backend, const char *path,
              struct bench_stat *out);
  int (*iterate)(struct bench_backend *backend, const char *path,
                 bench_dir_callback callback, void *opaque);
  int (*rename)(struct bench_backend *backend, const char *old_path,
                const char *new_path);
  int (*unlink)(struct bench_backend *backend, const char *path);
  int (*rmdir)(struct bench_backend *backend, const char *path);
  int (*flush)(struct bench_backend *backend);
  int (*space)(struct bench_backend *backend, struct bench_space *out);
};

struct bench_backend {
  const char *name;
  const struct bench_backend_ops *ops;
  void *private_data;
};

int bench_backend_create(struct bench_backend *backend);
void bench_backend_destroy(struct bench_backend *backend);

#endif
