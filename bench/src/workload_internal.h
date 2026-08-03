#ifndef BSFS_BENCH_WORKLOAD_INTERNAL_H
#define BSFS_BENCH_WORKLOAD_INTERNAL_H

#include "bench_result.h"
#include "bench_workload.h"
#include <sys/resource.h>

struct bench_phase_state {
  struct rusage usage;
  uint64_t started_ns;
  uint64_t sectors_read;
  uint64_t sectors_written;
  uint64_t free_blocks;
  int has_device_stats;
  int has_free_blocks;
};

void bench_phase_start(struct bench_run_config *config,
                       struct bench_phase_state *state);
/*
 * Times the durability barrier and marks the phase as covering one, so the
 * analyzer knows its device counters correspond to data that actually landed.
 */
int bench_phase_flush(struct bench_run_config *config,
                      struct bench_result *result);
int bench_phase_finish(struct bench_run_config *config,
                       const struct bench_phase_state *state,
                       struct bench_result *result);
void bench_result_error(struct bench_result *result, int error,
                        const char *context);
int bench_phase_selected(const struct bench_run_config *config,
                         const char *phase);

int bench_run_bulk(struct bench_run_config *config);
int bench_run_metadata(struct bench_run_config *config);
int bench_run_mixed(struct bench_run_config *config);
int bench_run_recovery(struct bench_run_config *config);
int bench_run_old_recovery(struct bench_run_config *config);
int bench_run_concurrency_seq(struct bench_run_config *config);
int bench_run_concurrency_mixed(struct bench_run_config *config);

#endif
