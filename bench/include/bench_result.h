#ifndef BSFS_BENCH_RESULT_H
#define BSFS_BENCH_RESULT_H

#include <stddef.h>
#include <stdint.h>

struct bench_run_config;

struct bench_latency {
  uint64_t *values;
  size_t count;
  size_t capacity;
};

struct bench_result {
  const char *scenario;
  const char *phase;
  uint64_t operations;
  uint64_t logical_bytes;
  uint64_t elapsed_ns;
  uint64_t operation_path_ns;
  uint64_t final_flush_ns;
  uint64_t wall_ns;
  uint64_t cpu_user_ns;
  uint64_t cpu_system_ns;
  uint64_t device_sectors_read;
  uint64_t device_sectors_written;
  uint64_t free_blocks_before;
  uint64_t free_blocks_after;
  uint64_t latency_p50_ns;
  uint64_t latency_p95_ns;
  uint64_t latency_p99_ns;
  uint64_t latency_max_ns;
  int validation_ok;
  int error_code;
  const char *error_message;
  const char *raw_latency_path;

  uint64_t reclaim_attempts;
  uint64_t reclaim_successes;
  uint64_t reclaim_entries_purged;
  uint64_t reclaim_blocks_reclaimed;
  uint64_t reclaim_pre_commit_ns;
  uint64_t reclaim_purge_ns;
  uint64_t reclaim_post_commit_ns;
  uint64_t reclaim_failures;
  int has_latency;
  int has_device_stats;
  int has_free_blocks;
  int has_reclaim_stats;
  int has_durable_window;
  int has_worker_id;
  int has_wall;
  unsigned int worker_id;
};

void bench_result_init(struct bench_result *result, const char *scenario,
                       const char *phase);
int bench_result_write(const struct bench_run_config *config,
                       const struct bench_result *result);

int bench_latency_init(struct bench_latency *latencies, size_t capacity);
int bench_latency_add(struct bench_latency *latencies, uint64_t value);
void bench_latency_summarize(struct bench_latency *latencies,
                             struct bench_result *result);
int bench_latency_write_raw(const struct bench_run_config *config,
                            const char *phase,
                            const struct bench_latency *latencies,
                            char *path, size_t path_size);
void bench_latency_destroy(struct bench_latency *latencies);

#endif
