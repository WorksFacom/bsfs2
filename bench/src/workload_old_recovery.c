#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OLD_RECOVERY_FILE_SIZE (64U * 1024U)

static uint32_t old_recovery_count(const struct bench_run_config *config) {
  return config->profile == BENCH_PROFILE_PUBLICATION ? 1000 : 64;
}

static int old_recovery_create(struct bench_run_config *config,
                               uint32_t count) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  unsigned char *buffer;
  char path[BENCH_PATH_MAX];
  int error;

  error = config->backend.ops->mkdir(&config->backend, "/old-recovery");
  if (error)
    return error;
  buffer = malloc(OLD_RECOVERY_FILE_SIZE);
  if (!buffer)
    return -ENOMEM;
  bench_result_init(&result, "old-recovery", "create");
  error = bench_latency_init(&latencies, count);
  if (error) {
    free(buffer);
    return error;
  }
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < count; i++) {
    struct bench_file file = {0};
    uint64_t started;
    uint64_t elapsed;

    error =
        bench_format_path(path, sizeof(path), "/old-recovery/file-%05u", i);
    if (error)
      break;
    bench_pattern_fill(buffer, OLD_RECOVERY_FILE_SIZE, config->seed,
                       0xb00000ULL + i, 0);
    started = bench_now_ns();
    error =
        config->backend.ops->create_file(&config->backend, path, &file);
    if (!error)
      error = bench_write_all(&config->backend, &file, buffer,
                              OLD_RECOVERY_FILE_SIZE, NULL);
    if (file.private_data)
      (void)config->backend.ops->close_file(&config->backend, &file);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operations++;
    result.logical_bytes += OLD_RECOVERY_FILE_SIZE;
    result.operation_path_ns += elapsed;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  bench_latency_summarize(&latencies, &result);
  if (error)
    bench_result_error(&result, error, "old-BSFS recovery setup failed");
  bench_latency_destroy(&latencies);
  free(buffer);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int old_recovery_delete(struct bench_run_config *config,
                               uint32_t count) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  char path[BENCH_PATH_MAX];
  int error;

  bench_result_init(&result, "old-recovery",
                    "native-retention-logical-deletion");
  error = bench_latency_init(&latencies, count);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < count; i++) {
    uint64_t started;
    uint64_t elapsed;

    error =
        bench_format_path(path, sizeof(path), "/old-recovery/file-%05u", i);
    if (error)
      break;
    started = bench_now_ns();
    error = config->backend.ops->unlink(&config->backend, path);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operations++;
    result.operation_path_ns += elapsed;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  bench_latency_summarize(&latencies, &result);
  if (error)
    bench_result_error(&result, error, "old-BSFS logical deletion failed");
  bench_latency_destroy(&latencies);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int old_recovery_prepare(struct bench_run_config *config,
                                uint32_t count) {
  int error = old_recovery_create(config, count);

  if (error)
    return error;
  return old_recovery_delete(config, count);
}

static int list_recovery_ids(struct bench_run_config *config, uint32_t count,
                             struct bench_recovery_entry **out_entries,
                             size_t *out_count) {
  struct bench_recovery_entry *entries = calloc(count, sizeof(*entries));
  size_t found = 0;
  int error;

  if (!entries)
    return -ENOMEM;
  error = config->backend.ops->recovery_list(&config->backend, entries, count,
                                             &found);
  if (error) {
    free(entries);
    return error;
  }
  *out_entries = entries;
  *out_count = found;
  return 0;
}

/* the pattern stream is keyed off the file's own index, not its listing slot */
static int recovery_file_index(const char *name, uint32_t *index) {
  unsigned int parsed;
  char extra;

  if (sscanf(name, "file-%05u%c", &parsed, &extra) != 1)
    return -EILSEQ;
  *index = parsed;
  return 0;
}

static int old_recovery_retention(struct bench_run_config *config,
                                  uint32_t count) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_recovery_entry *entries = NULL;
  size_t found = 0;
  int error;

  bench_result_init(&result, "old-recovery", "retention-verify");
  bench_phase_start(config, &state);
  error = list_recovery_ids(config, count, &entries, &found);
  if (!error && found != count)
    error = -EILSEQ;
  result.operations = found;
  if (error)
    bench_result_error(&result, error,
                       "old-BSFS retained recovery entry count differs");
  free(entries);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int old_recovery_restore(struct bench_run_config *config,
                                uint32_t count) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct bench_recovery_entry *entries = NULL;
  size_t found = 0;
  uint32_t restore_count = count < 100 ? count / 4 : 100;
  unsigned char *buffer;
  unsigned char *expected;
  char path[BENCH_PATH_MAX];
  int error;

  if (restore_count == 0)
    restore_count = 1;
  error = list_recovery_ids(config, count, &entries, &found);
  if (error)
    return error;
  if (found < restore_count) {
    free(entries);
    return -EILSEQ;
  }
  buffer = malloc(OLD_RECOVERY_FILE_SIZE);
  expected = malloc(OLD_RECOVERY_FILE_SIZE);
  if (!buffer || !expected) {
    free(entries);
    free(buffer);
    free(expected);
    return -ENOMEM;
  }
  bench_result_init(&result, "old-recovery", "restore-fixed-subset");
  error = bench_latency_init(&latencies, restore_count);
  if (error)
    goto out;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < restore_count; i++) {
    struct bench_file file = {0};
    uint64_t started;
    uint64_t elapsed;
    uint32_t index;

    error = recovery_file_index(entries[i].name, &index);
    if (error)
      break;
    started = bench_now_ns();
    error = config->backend.ops->recovery_restore(
        &config->backend, entries[i].id, "/old-recovery");
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    error = bench_format_path(path, sizeof(path), "/old-recovery/%s",
                              entries[i].name);
    if (!error)
      error =
          config->backend.ops->open_file(&config->backend, path, 0, &file);
    if (!error)
      error = bench_read_all(&config->backend, &file, buffer,
                             OLD_RECOVERY_FILE_SIZE, NULL);
    if (file.private_data)
      (void)config->backend.ops->close_file(&config->backend, &file);
    if (error)
      break;
    bench_pattern_fill(expected, OLD_RECOVERY_FILE_SIZE, config->seed,
                       0xb00000ULL + index, 0);
    if (memcmp(buffer, expected, OLD_RECOVERY_FILE_SIZE) != 0) {
      error = -EILSEQ;
      break;
    }
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operations++;
    result.logical_bytes += OLD_RECOVERY_FILE_SIZE;
    result.operation_path_ns += elapsed;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  bench_latency_summarize(&latencies, &result);
  if (error)
    bench_result_error(&result, error, "old-BSFS restore validation failed");
  bench_latency_destroy(&latencies);
  free(entries);
  free(buffer);
  free(expected);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }

out:
  free(entries);
  free(buffer);
  free(expected);
  return error;
}

static int old_recovery_purge(struct bench_run_config *config) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_space space = {0};
  uint64_t reclaimed = 0;
  int error;

  bench_result_init(&result, "old-recovery", "explicit-purge");
  bench_phase_start(config, &state);
  error = config->backend.ops->space(&config->backend, &space);
  if (!error) {
    uint64_t started = bench_now_ns();
    error = config->backend.ops->recovery_purge(
        &config->backend, space.total_blocks, &reclaimed);
    result.operation_path_ns = bench_now_ns() - started;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  result.operations = 1;
  result.logical_bytes = reclaimed * space.block_size;
  result.elapsed_ns = result.operation_path_ns;
  result.reclaim_blocks_reclaimed = reclaimed;
  result.has_reclaim_stats = 1;
  if (error)
    bench_result_error(&result, error, "old-BSFS explicit purge failed");
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

int bench_run_old_recovery(struct bench_run_config *config) {
  uint32_t count = old_recovery_count(config);
  int error;

  if (bench_phase_selected(config, "prepare") &&
      (error = old_recovery_prepare(config, count)) != 0)
    return error;
  if (bench_phase_selected(config, "retention") &&
      (error = old_recovery_retention(config, count)) != 0)
    return error;
  if (bench_phase_selected(config, "restore") &&
      (error = old_recovery_restore(config, count)) != 0)
    return error;
  if (bench_phase_selected(config, "purge") &&
      (error = old_recovery_purge(config)) != 0)
    return error;
  return 0;
}
