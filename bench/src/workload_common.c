#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void bench_phase_start(struct bench_run_config *config,
                       struct bench_phase_state *state) {
  struct bench_space space;

  memset(state, 0, sizeof(*state));
  bench_cpu_usage(&state->usage);
  state->started_ns = bench_now_ns();
  if (config->block_device &&
      bench_device_sectors(config->block_device, &state->sectors_read,
                           &state->sectors_written) == 0)
    state->has_device_stats = 1;
  if (config->backend.ops->space(&config->backend, &space) == 0) {
    state->free_blocks = space.free_blocks;
    state->has_free_blocks = 1;
  }
}

int bench_phase_flush(struct bench_run_config *config,
                      struct bench_result *result) {
  uint64_t started = bench_now_ns();
  int error = config->backend.ops->flush(&config->backend);

  result->final_flush_ns = bench_now_ns() - started;
  result->has_durable_window = 1;
  return error;
}

int bench_phase_finish(struct bench_run_config *config,
                       const struct bench_phase_state *state,
                       struct bench_result *result) {
  struct rusage usage;
  struct bench_space space;
  uint64_t read_sectors = 0;
  uint64_t written_sectors = 0;

  if (result->elapsed_ns == 0)
    result->elapsed_ns = bench_now_ns() - state->started_ns;
  bench_cpu_usage(&usage);
  result->cpu_user_ns =
      bench_timespec_ns(usage.ru_utime.tv_sec, usage.ru_utime.tv_usec) -
      bench_timespec_ns(state->usage.ru_utime.tv_sec,
                        state->usage.ru_utime.tv_usec);
  result->cpu_system_ns =
      bench_timespec_ns(usage.ru_stime.tv_sec, usage.ru_stime.tv_usec) -
      bench_timespec_ns(state->usage.ru_stime.tv_sec,
                        state->usage.ru_stime.tv_usec);
  result->free_blocks_before = state->free_blocks;
  if (state->has_free_blocks &&
      config->backend.ops->space(&config->backend, &space) == 0) {
    result->free_blocks_after = space.free_blocks;
    result->has_free_blocks = 1;
  }
  if (state->has_device_stats &&
      bench_device_sectors(config->block_device, &read_sectors,
                           &written_sectors) == 0) {
    result->device_sectors_read = read_sectors - state->sectors_read;
    result->device_sectors_written =
        written_sectors - state->sectors_written;
    result->has_device_stats = 1;
  }
  return bench_result_write(config, result);
}

void bench_result_error(struct bench_result *result, int error,
                        const char *context) {
  result->validation_ok = 0;
  result->error_code = error < 0 ? -error : error;
  result->error_message = context ? context : strerror(result->error_code);
}

int bench_phase_selected(const struct bench_run_config *config,
                         const char *phase) {
  return strcmp(config->phase, "all") == 0 ||
         strcmp(config->phase, phase) == 0;
}
