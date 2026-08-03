#ifndef BSFS_BENCH_UTIL_H
#define BSFS_BENCH_UTIL_H

#include "bench_backend.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>

uint64_t bench_now_ns(void);
uint64_t bench_timespec_ns(long seconds, long microseconds);
void bench_cpu_usage(struct rusage *usage);
void bench_pattern_fill(void *buffer, size_t length, uint64_t seed,
                        uint64_t stream, uint64_t offset);
uint64_t bench_splitmix64(uint64_t value);
void bench_shuffle_u32(uint32_t *values, size_t count, uint64_t seed);

int bench_write_all(struct bench_backend *backend, struct bench_file *file,
                    const void *buffer, size_t length, uint64_t *elapsed_ns);
int bench_read_all(struct bench_backend *backend, struct bench_file *file,
                   void *buffer, size_t length, uint64_t *elapsed_ns);
int bench_format_path(char *buffer, size_t capacity, const char *format, ...);

int bench_device_sectors(const char *device, uint64_t *read_sectors,
                         uint64_t *written_sectors);

#endif
