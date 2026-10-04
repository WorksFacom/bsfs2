#ifndef BSFS_BENCH_WORKLOAD_H
#define BSFS_BENCH_WORKLOAD_H

#include "bench_backend.h"
#include <stddef.h>
#include <stdint.h>

#define BENCH_PATH_MAX 4096
#define BENCH_ID_MAX 128

struct bench_run_config {
  struct bench_backend backend;
  const char *target;
  const char *scenario;
  const char *phase;
  const char *result_path;
  const char *artifact_dir;
  const char *block_device;
  const char *reclaim_stats_path;
  const char *filesystem;
  const char *filesystem_version;
  const char *filesystem_options;
  const char *run_id;
  uint64_t seed;
  unsigned int repetition;
  unsigned int worker_count;
  unsigned int cpu_count;
  unsigned int cpus[256];
};

int bench_run_workload(struct bench_run_config *config);

#endif
