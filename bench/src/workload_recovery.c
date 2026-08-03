#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOVERY_FILE_SIZE (64U * 1024U)
#define REFILL_FILE_SIZE (1024U * 1024U)
#define SPACE_SAMPLE_INTERVAL 16U

struct reclaim_snapshot {
  uint64_t attempts;
  uint64_t successes;
  uint64_t entries_purged;
  uint64_t blocks_reclaimed;
  uint64_t pre_commit_ns;
  uint64_t purge_ns;
  uint64_t post_commit_ns;
  uint64_t failures;
};

struct survivor_scan {
  unsigned char *seen;
  size_t capacity;
  size_t count;
  uint32_t max_index;
  int error;
};

static uint32_t recovery_seed_files(const struct bench_run_config *config) {
  return config->profile == BENCH_PROFILE_PUBLICATION ? 1000 : 64;
}

static void reclaim_accumulate(struct bench_result *result,
                               const struct reclaim_snapshot *before,
                               const struct reclaim_snapshot *after) {
  result->has_reclaim_stats = 1;
  result->reclaim_attempts = after->attempts - before->attempts;
  result->reclaim_successes = after->successes - before->successes;
  result->reclaim_entries_purged =
      after->entries_purged - before->entries_purged;
  result->reclaim_blocks_reclaimed =
      after->blocks_reclaimed - before->blocks_reclaimed;
  result->reclaim_pre_commit_ns =
      after->pre_commit_ns - before->pre_commit_ns;
  result->reclaim_purge_ns = after->purge_ns - before->purge_ns;
  result->reclaim_post_commit_ns =
      after->post_commit_ns - before->post_commit_ns;
  result->reclaim_failures = after->failures - before->failures;
}

static int read_reclaim_snapshot(const char *path,
                                 struct reclaim_snapshot *snapshot) {
  FILE *input;
  char key[80];
  unsigned long long value;

  memset(snapshot, 0, sizeof(*snapshot));
  if (!path)
    return -ENOENT;
  input = fopen(path, "r");
  if (!input)
    return -errno;
  while (fscanf(input, "%79s %llu", key, &value) == 2) {
    if (strcmp(key, "attempts") == 0)
      snapshot->attempts = value;
    else if (strcmp(key, "successes") == 0)
      snapshot->successes = value;
    else if (strcmp(key, "entries_purged") == 0)
      snapshot->entries_purged = value;
    else if (strcmp(key, "blocks_reclaimed") == 0)
      snapshot->blocks_reclaimed = value;
    else if (strcmp(key, "pre_commit_ns") == 0)
      snapshot->pre_commit_ns = value;
    else if (strcmp(key, "purge_ns") == 0)
      snapshot->purge_ns = value;
    else if (strcmp(key, "post_commit_ns") == 0)
      snapshot->post_commit_ns = value;
    else if (strcmp(key, "failures") == 0)
      snapshot->failures = value;
  }
  if (ferror(input)) {
    fclose(input);
    return -EIO;
  }
  fclose(input);
  return 0;
}

static int create_recovery_seed(struct bench_run_config *config,
                                uint32_t count) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  unsigned char *buffer;
  char path[BENCH_PATH_MAX];
  int error;

  error = config->backend.ops->mkdir(&config->backend, "/recovery-pressure");
  if (error)
    return error;
  buffer = malloc(RECOVERY_FILE_SIZE);
  if (!buffer)
    return -ENOMEM;
  bench_result_init(&result, "recovery-pressure", "seed-create");
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

    error = bench_format_path(path, sizeof(path),
                              "/recovery-pressure/seed-%05u", i);
    if (error)
      break;
    bench_pattern_fill(buffer, RECOVERY_FILE_SIZE, config->seed,
                       0x500000ULL + i, 0);
    started = bench_now_ns();
    error =
        config->backend.ops->create_file(&config->backend, path, &file);
    if (!error)
      error = bench_write_all(&config->backend, &file, buffer,
                              RECOVERY_FILE_SIZE, NULL);
    if (file.private_data)
      (void)config->backend.ops->close_file(&config->backend, &file);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operations++;
    result.logical_bytes += RECOVERY_FILE_SIZE;
    result.operation_path_ns += elapsed;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  bench_latency_summarize(&latencies, &result);
  if (error)
    bench_result_error(&result, error, "recovery seed creation failed");
  free(buffer);
  bench_latency_destroy(&latencies);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int delete_recovery_files(struct bench_run_config *config,
                                 const char *kind, uint32_t cycle,
                                 uint32_t count,
                                 const struct reclaim_snapshot *stats_before) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct reclaim_snapshot stats_after;
  char path[BENCH_PATH_MAX];
  char phase[80];
  int error;

  if (strcmp(kind, "seed") == 0)
    snprintf(phase, sizeof(phase), "seed-native-namespace-removal");
  else
    snprintf(phase, sizeof(phase), "cycle-%u-native-namespace-removal",
             cycle + 1);
  bench_result_init(&result, "recovery-pressure", phase);
  error = bench_latency_init(&latencies, count);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < count; i++) {
    uint64_t started;
    uint64_t elapsed;

    if (strcmp(kind, "seed") == 0)
      error = bench_format_path(path, sizeof(path),
                                "/recovery-pressure/seed-%05u", i);
    else
      error = bench_format_path(path, sizeof(path),
                                "/recovery-pressure/refill-c%u-%08u", cycle,
                                i);
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
  if (read_reclaim_snapshot(config->reclaim_stats_path, &stats_after) == 0)
    reclaim_accumulate(&result, stats_before, &stats_after);
  if (error)
    bench_result_error(&result, error, "recovery namespace removal failed");
  bench_latency_destroy(&latencies);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static FILE *open_recovery_timeline(const struct bench_run_config *config,
                                    char *path, size_t capacity) {
  if (!config->artifact_dir)
    return NULL;
  if (bench_format_path(path, capacity,
                        "%s/%s-recovery-timeline-r%u.jsonl",
                        config->artifact_dir, config->filesystem,
                        config->repetition) != 0)
    return NULL;
  return fopen(path, "a");
}

static void timeline_write(FILE *timeline, uint32_t cycle, uint32_t index,
                           uint64_t latency_ns, uint64_t free_blocks,
                           const struct reclaim_snapshot *stats, int error) {
  if (!timeline)
    return;
  fprintf(timeline,
          "{\"cycle\":%u,\"index\":%u,\"latency_ns\":%" PRIu64
          ",\"free_blocks\":%" PRIu64 ",\"reclaim_attempts\":%" PRIu64
          ",\"reclaim_successes\":%" PRIu64
          ",\"entries_purged\":%" PRIu64
          ",\"blocks_reclaimed\":%" PRIu64 ",\"error_code\":%d}\n",
          cycle + 1, index, latency_ns, free_blocks, stats->attempts,
          stats->successes, stats->entries_purged, stats->blocks_reclaimed,
          error < 0 ? -error : error);
  fflush(timeline);
}

static int recovery_refill_cycle(struct bench_run_config *config,
                                 uint32_t cycle, uint64_t target_blocks,
                                 uint32_t *files_written) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct reclaim_snapshot stats_before;
  struct reclaim_snapshot stats_now;
  struct bench_space space;
  unsigned char *buffer;
  char path[BENCH_PATH_MAX];
  char phase[80];
  char raw_path[BENCH_PATH_MAX] = {0};
  char timeline_path[BENCH_PATH_MAX] = {0};
  FILE *timeline;
  uint64_t max_files;
  int error = 0;
  int expected_enospc = 0;

  if (config->backend.ops->space(&config->backend, &space) != 0)
    return -EIO;
  max_files =
      (space.total_blocks * space.block_size) / REFILL_FILE_SIZE + 1024;
  buffer = malloc(REFILL_FILE_SIZE);
  if (!buffer)
    return -ENOMEM;
  snprintf(phase, sizeof(phase), "cycle-%u-refill", cycle + 1);
  bench_result_init(&result, "recovery-pressure", phase);
  error = bench_latency_init(&latencies,
                             max_files < SIZE_MAX ? (size_t)max_files : 0);
  if (error) {
    free(buffer);
    return error;
  }
  if (read_reclaim_snapshot(config->reclaim_stats_path, &stats_before) != 0)
    memset(&stats_before, 0, sizeof(stats_before));
  timeline = open_recovery_timeline(config, timeline_path,
                                    sizeof(timeline_path));
  bench_phase_start(config, &state);

  for (uint32_t i = 0; (uint64_t)i < max_files; i++) {
    struct bench_file file = {0};
    uint64_t started;
    uint64_t elapsed;
    uint64_t free_blocks = 0;

    error = bench_format_path(path, sizeof(path),
                              "/recovery-pressure/refill-c%u-%08u", cycle, i);
    if (error)
      break;
    bench_pattern_fill(buffer, REFILL_FILE_SIZE, config->seed,
                       0x600000000ULL + ((uint64_t)cycle << 32) + i, 0);
    started = bench_now_ns();
    error =
        config->backend.ops->create_file(&config->backend, path, &file);
    if (!error)
      error = bench_write_all(&config->backend, &file, buffer,
                              REFILL_FILE_SIZE, NULL);
    if (file.private_data)
      (void)config->backend.ops->close_file(&config->backend, &file);
    elapsed = bench_now_ns() - started;
    if (error == -ENOSPC) {
      expected_enospc = 1;
      (void)config->backend.ops->unlink(&config->backend, path);
    } else if (!error) {
      (*files_written)++;
      result.operations++;
      result.logical_bytes += REFILL_FILE_SIZE;
      result.operation_path_ns += elapsed;
      error = bench_latency_add(&latencies, elapsed);
    }
    /*
     * space() is a full bitmap scan on the userspace backend, so sampling it
     * every iteration would pollute the page cache and the device counters
     * this phase is measuring.
     */
    if (i % SPACE_SAMPLE_INTERVAL == 0 &&
        config->backend.ops->space(&config->backend, &space) == 0)
      free_blocks = space.free_blocks;
    if (read_reclaim_snapshot(config->reclaim_stats_path, &stats_now) != 0)
      memset(&stats_now, 0, sizeof(stats_now));
    timeline_write(timeline, cycle, i, elapsed, free_blocks, &stats_now, error);
    if (error || expected_enospc)
      break;
    if (stats_now.blocks_reclaimed - stats_before.blocks_reclaimed >=
        target_blocks)
      break;
  }
  if (timeline)
    fclose(timeline);
  if (!error || expected_enospc) {
    int flush_error = bench_phase_flush(config, &result);
    if (!expected_enospc)
      error = flush_error;
  }
  result.elapsed_ns = result.operation_path_ns;
  bench_latency_summarize(&latencies, &result);
  if (bench_latency_write_raw(config, result.phase, &latencies, raw_path,
                              sizeof(raw_path)) == 0 &&
      raw_path[0])
    result.raw_latency_path = raw_path;
  if (read_reclaim_snapshot(config->reclaim_stats_path, &stats_now) == 0)
    reclaim_accumulate(&result, &stats_before, &stats_now);
  if (!expected_enospc && result.reclaim_blocks_reclaimed < target_blocks)
    error = error ? error : -ENOSPC;
  if (error && !expected_enospc)
    bench_result_error(&result, error, "recovery refill failed");
  free(buffer);
  bench_latency_destroy(&latencies);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return (error && !expected_enospc) ? error : output_error;
  }
}

static int recovery_run(struct bench_run_config *config) {
  uint32_t seed_files = recovery_seed_files(config);
  uint64_t target_blocks;
  struct bench_space space;
  struct reclaim_snapshot stats;
  int error;

  error = create_recovery_seed(config, seed_files);
  if (error)
    return error;
  if (read_reclaim_snapshot(config->reclaim_stats_path, &stats) != 0)
    memset(&stats, 0, sizeof(stats));
  error = delete_recovery_files(config, "seed", 0, seed_files, &stats);
  if (error)
    return error;
  if (config->backend.ops->space(&config->backend, &space) != 0)
    return -EIO;
  target_blocks =
      ((uint64_t)seed_files * RECOVERY_FILE_SIZE) / space.block_size;

  for (uint32_t cycle = 0; cycle < 3; cycle++) {
    uint32_t files_written = 0;

    error = recovery_refill_cycle(config, cycle, target_blocks, &files_written);
    if (error)
      return error;
    if (files_written == 0)
      return -ENOSPC;
    if (cycle == 2)
      break;
    if (read_reclaim_snapshot(config->reclaim_stats_path, &stats) != 0)
      memset(&stats, 0, sizeof(stats));
    error = delete_recovery_files(config, "refill", cycle, files_written,
                                  &stats);
    if (error)
      return error;
    target_blocks =
        ((uint64_t)files_written * REFILL_FILE_SIZE) / space.block_size;
  }
  return 0;
}

static int survivor_callback(const char *name, void *opaque) {
  struct survivor_scan *scan = opaque;
  unsigned int cycle;
  unsigned int index;

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return 0;
  if (sscanf(name, "refill-c%u-%u", &cycle, &index) != 2 || cycle != 2)
    return -EILSEQ;
  if ((size_t)index >= scan->capacity) {
    size_t new_capacity = scan->capacity ? scan->capacity : 1024;
    unsigned char *grown;

    while (new_capacity <= index)
      new_capacity *= 2;
    grown = realloc(scan->seen, new_capacity);
    if (!grown)
      return -ENOMEM;
    memset(grown + scan->capacity, 0, new_capacity - scan->capacity);
    scan->seen = grown;
    scan->capacity = new_capacity;
  }
  if (scan->seen[index])
    return -EEXIST;
  scan->seen[index] = 1;
  scan->count++;
  if (index > scan->max_index)
    scan->max_index = index;
  return 0;
}

static int recovery_verify(struct bench_run_config *config) {
  struct bench_phase_state state;
  struct bench_result result;
  struct survivor_scan scan = {0};
  unsigned char *buffer;
  unsigned char *expected;
  char path[BENCH_PATH_MAX];
  int error;

  bench_result_init(&result, "recovery-pressure", "verify-survivors");
  buffer = malloc(REFILL_FILE_SIZE);
  expected = malloc(REFILL_FILE_SIZE);
  if (!buffer || !expected) {
    free(buffer);
    free(expected);
    return -ENOMEM;
  }
  bench_phase_start(config, &state);
  error = config->backend.ops->iterate(&config->backend, "/recovery-pressure",
                                       survivor_callback, &scan);
  /* an empty survivor set leaves scan.seen NULL, so never enter the loop */
  if (!error && scan.count == 0)
    error = -EILSEQ;
  for (uint32_t i = 0; !error && i <= scan.max_index; i++) {
    struct bench_file file = {0};
    uint64_t started;

    if (!scan.seen[i]) {
      error = -EILSEQ;
      break;
    }
    error = bench_format_path(path, sizeof(path),
                              "/recovery-pressure/refill-c2-%08u", i);
    if (!error)
      error =
          config->backend.ops->open_file(&config->backend, path, 0, &file);
    if (error)
      break;
    started = bench_now_ns();
    error = bench_read_all(&config->backend, &file, buffer, REFILL_FILE_SIZE,
                           NULL);
    result.operation_path_ns += bench_now_ns() - started;
    (void)config->backend.ops->close_file(&config->backend, &file);
    if (error)
      break;
    bench_pattern_fill(expected, REFILL_FILE_SIZE, config->seed,
                       0x600000000ULL + (2ULL << 32) + i, 0);
    if (memcmp(buffer, expected, REFILL_FILE_SIZE) != 0) {
      error = -EILSEQ;
      break;
    }
    result.operations++;
    result.logical_bytes += REFILL_FILE_SIZE;
  }
  result.elapsed_ns = result.operation_path_ns;
  if (error)
    bench_result_error(&result, error, "recovery survivor validation failed");
  free(scan.seen);
  free(buffer);
  free(expected);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

int bench_run_recovery(struct bench_run_config *config) {
  int error;

  if (bench_phase_selected(config, "run") &&
      (error = recovery_run(config)) != 0)
    return error;
  if (bench_phase_selected(config, "verify") &&
      (error = recovery_verify(config)) != 0)
    return error;
  return 0;
}
