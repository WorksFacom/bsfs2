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

#define BENCH_RECOVERY_NAME_MAX 256

struct bench_recovery_entry {
  uint32_t id;
  char name[BENCH_RECOVERY_NAME_MAX];
};

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

  /*
   * Recovery operations are optional. They return -EOPNOTSUPP when the
   * selected backend does not expose a corresponding native operation.
   */
  int (*recovery_list)(struct bench_backend *backend,
                       struct bench_recovery_entry *entries, size_t capacity,
                       size_t *count);
  int (*recovery_restore)(struct bench_backend *backend, uint32_t id,
                          const char *destination);
  int (*recovery_purge)(struct bench_backend *backend, uint64_t blocks,
                        uint64_t *blocks_reclaimed);
};

struct bench_backend {
  const char *name;
  const struct bench_backend_ops *ops;
  void *private_data;
};

/* Exactly one implementation is linked into each runner executable. */
int bench_backend_create(struct bench_backend *backend);
void bench_backend_destroy(struct bench_backend *backend);

#endif
