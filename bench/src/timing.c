#define _GNU_SOURCE
#include "bench_util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t bench_now_ns(void) {
  struct timespec ts;

  /* returning 0 here would silently wrap every elapsed-time subtraction */
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    perror("clock_gettime");
    abort();
  }
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t bench_timespec_ns(long seconds, long microseconds) {
  return (uint64_t)seconds * 1000000000ULL + (uint64_t)microseconds * 1000ULL;
}

void bench_cpu_usage(struct rusage *usage) {
  memset(usage, 0, sizeof(*usage));
  (void)getrusage(RUSAGE_SELF, usage);
}

int bench_write_all(struct bench_backend *backend, struct bench_file *file,
                    const void *buffer, size_t length, uint64_t *elapsed_ns) {
  const unsigned char *cursor = buffer;
  size_t remaining = length;
  uint64_t start = bench_now_ns();

  while (remaining > 0) {
    ssize_t written =
        backend->ops->write_file(backend, file, cursor, remaining);
    if (written < 0)
      return (int)written;
    if (written == 0)
      return -EIO;
    cursor += (size_t)written;
    remaining -= (size_t)written;
  }

  if (elapsed_ns)
    *elapsed_ns += bench_now_ns() - start;
  return 0;
}

int bench_read_all(struct bench_backend *backend, struct bench_file *file,
                   void *buffer, size_t length, uint64_t *elapsed_ns) {
  unsigned char *cursor = buffer;
  size_t remaining = length;
  uint64_t start = bench_now_ns();

  while (remaining > 0) {
    ssize_t received =
        backend->ops->read_file(backend, file, cursor, remaining);
    if (received < 0)
      return (int)received;
    if (received == 0)
      return -ENODATA;
    cursor += (size_t)received;
    remaining -= (size_t)received;
  }

  if (elapsed_ns)
    *elapsed_ns += bench_now_ns() - start;
  return 0;
}

int bench_format_path(char *buffer, size_t capacity, const char *format, ...) {
  va_list args;
  int written;

  va_start(args, format);
  written = vsnprintf(buffer, capacity, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= capacity)
    return -ENAMETOOLONG;
  return 0;
}
