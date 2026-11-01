#define _GNU_SOURCE
#include "bench_result.h"
#include "bench_util.h"
#include "bench_workload.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int compare_u64(const void *left, const void *right) {
  const uint64_t a = *(const uint64_t *)left;
  const uint64_t b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

/* nearest-rank rounding up, so p99 of 100 samples is the max. matches fio */
static uint64_t percentile(const uint64_t *values, size_t count,
                           unsigned int percent) {
  size_t index;

  if (count == 0)
    return 0;
  index = ((count - 1) * percent + 99) / 100;
  return values[index];
}

static void json_string(FILE *output, const char *value) {
  const unsigned char *cursor;

  if (!value) {
    fputs("null", output);
    return;
  }

  fputc('"', output);
  for (cursor = (const unsigned char *)value; *cursor; cursor++) {
    switch (*cursor) {
    case '"':
      fputs("\\\"", output);
      break;
    case '\\':
      fputs("\\\\", output);
      break;
    case '\n':
      fputs("\\n", output);
      break;
    case '\r':
      fputs("\\r", output);
      break;
    case '\t':
      fputs("\\t", output);
      break;
    default:
      if (*cursor < 0x20)
        fprintf(output, "\\u%04x", *cursor);
      else
        fputc(*cursor, output);
    }
  }
  fputc('"', output);
}

void bench_result_init(struct bench_result *result, const char *scenario,
                       const char *phase) {
  memset(result, 0, sizeof(*result));
  result->scenario = scenario;
  result->phase = phase;
  result->validation_ok = 1;
}

int bench_latency_init(struct bench_latency *latencies, size_t capacity) {
  memset(latencies, 0, sizeof(*latencies));
  if (capacity == 0)
    return 0;
  latencies->values = calloc(capacity, sizeof(*latencies->values));
  if (!latencies->values)
    return -ENOMEM;
  latencies->capacity = capacity;
  return 0;
}

int bench_latency_add(struct bench_latency *latencies, uint64_t value) {
  uint64_t *grown;
  size_t capacity;

  if (latencies->count == latencies->capacity) {
    capacity = latencies->capacity ? latencies->capacity * 2 : 256;
    grown = realloc(latencies->values, capacity * sizeof(*grown));
    if (!grown)
      return -ENOMEM;
    latencies->values = grown;
    latencies->capacity = capacity;
  }
  latencies->values[latencies->count++] = value;
  return 0;
}

void bench_latency_summarize(struct bench_latency *latencies,
                             struct bench_result *result) {
  uint64_t *sorted;

  if (!latencies || latencies->count == 0)
    return;
  sorted = malloc(latencies->count * sizeof(*sorted));
  if (!sorted)
    return;
  memcpy(sorted, latencies->values, latencies->count * sizeof(*sorted));
  qsort(sorted, latencies->count, sizeof(*sorted), compare_u64);
  result->latency_p50_ns = percentile(sorted, latencies->count, 50);
  result->latency_p95_ns = percentile(sorted, latencies->count, 95);
  result->latency_p99_ns = percentile(sorted, latencies->count, 99);
  result->latency_max_ns = sorted[latencies->count - 1];
  result->has_latency = 1;
  free(sorted);
}

int bench_latency_write_raw(const struct bench_run_config *config,
                            const char *phase,
                            const struct bench_latency *latencies, char *path,
                            size_t path_size) {
  FILE *output;
  int error;

  if (!config->artifact_dir || !latencies || latencies->count == 0)
    return 0;
  error = bench_format_path(
      path, path_size, "%s/%s-%s-%s-r%u-w%u-x%u.latency-u64",
      config->artifact_dir, config->filesystem, config->scenario, phase,
      config->repetition, config->worker_count, config->work_scale);
  if (error)
    return error;
  output = fopen(path, "wb");
  if (!output)
    return -errno;
  if (fwrite(latencies->values, sizeof(*latencies->values), latencies->count,
             output) != latencies->count) {
    error = -EIO;
  } else {
    error = 0;
  }
  if (fclose(output) != 0 && error == 0)
    error = -errno;
  return error;
}

void bench_latency_destroy(struct bench_latency *latencies) {
  free(latencies->values);
  memset(latencies, 0, sizeof(*latencies));
}

int bench_result_write(const struct bench_run_config *config,
                       const struct bench_result *result) {
  FILE *output;
  time_t now;
  struct tm tm;
  char timestamp[40];
  double seconds = (double)result->elapsed_ns / 1000000000.0;
  double mib = (double)result->logical_bytes / (1024.0 * 1024.0);
  uint64_t durable_total = result->operation_path_ns + result->final_flush_ns;
  double durable_seconds = (double)durable_total / 1000000000.0;

  output = fopen(config->result_path, "a");
  if (!output)
    return -errno;

  now = time(NULL);
  gmtime_r(&now, &tm);
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm);

  fputs("{\"schema_version\":2,\"run_id\":", output);
  json_string(output, config->run_id);
  fputs(",\"timestamp\":", output);
  json_string(output, timestamp);
  fputs(",\"filesystem\":", output);
  json_string(output, config->filesystem);
  fputs(",\"filesystem_version\":", output);
  json_string(output, config->filesystem_version);
  fputs(",\"filesystem_options\":", output);
  json_string(output, config->filesystem_options);
  fputs(",\"scenario\":", output);
  json_string(output, result->scenario);
  fputs(",\"phase\":", output);
  json_string(output, result->phase);
  fprintf(output,
          ",\"repetition\":%u,\"seed\":%" PRIu64
          ",\"worker_count\":%u,\"work_scale\":%u,\"operations\":%" PRIu64
          ",\"logical_bytes\":%" PRIu64 ",\"elapsed_ns\":%" PRIu64
          ",\"operation_path_ns\":%" PRIu64 ",\"final_flush_ns\":%" PRIu64
          ",\"cpu_user_ns\":%" PRIu64 ",\"cpu_system_ns\":%" PRIu64,
          config->repetition, config->seed, config->worker_count,
          config->work_scale, result->operations, result->logical_bytes, result->elapsed_ns,
          result->operation_path_ns, result->final_flush_ns,
          result->cpu_user_ns, result->cpu_system_ns);
  if (result->has_wall)
    fprintf(output, ",\"wall_ns\":%" PRIu64, result->wall_ns);
  else
    fputs(",\"wall_ns\":null", output);
  if (result->has_worker_id)
    fprintf(output, ",\"worker_id\":%u", result->worker_id);
  else
    fputs(",\"worker_id\":null", output);

  if (seconds > 0.0) {
    fprintf(output, ",\"throughput_mib_s\":%.9f,\"iops\":%.9f",
            mib / seconds, (double)result->operations / seconds);
  } else {
    fputs(",\"throughput_mib_s\":null,\"iops\":null", output);
  }
  /*
   * throughput_mib_s excludes the flush, so buffered writes measure page-cache ingest.
   * durable rates include the flush, allowing comparisons across filesystems
   * that defer writes differently. Without a flush, these rates are null.
   */
  if (result->has_durable_window && durable_seconds > 0.0) {
    fprintf(output,
            ",\"durable_total_ns\":%" PRIu64
            ",\"durable_throughput_mib_s\":%.9f,\"durable_iops\":%.9f",
            durable_total, mib / durable_seconds,
            (double)result->operations / durable_seconds);
  } else {
    fputs(",\"durable_total_ns\":null,\"durable_throughput_mib_s\":null,"
          "\"durable_iops\":null",
          output);
  }

  if (result->has_latency)
    fprintf(output,
            ",\"latency_p50_ns\":%" PRIu64
            ",\"latency_p95_ns\":%" PRIu64
            ",\"latency_p99_ns\":%" PRIu64
            ",\"latency_max_ns\":%" PRIu64,
            result->latency_p50_ns, result->latency_p95_ns,
            result->latency_p99_ns, result->latency_max_ns);
  else
    fputs(",\"latency_p50_ns\":null,\"latency_p95_ns\":null,"
          "\"latency_p99_ns\":null,\"latency_max_ns\":null",
          output);
  if (result->has_device_stats)
    fprintf(output,
            ",\"device_sectors_read\":%" PRIu64
            ",\"device_sectors_written\":%" PRIu64,
            result->device_sectors_read, result->device_sectors_written);
  else
    fputs(",\"device_sectors_read\":null,"
          "\"device_sectors_written\":null",
          output);
  if (result->has_free_blocks)
    fprintf(output,
            ",\"free_blocks_before\":%" PRIu64
            ",\"free_blocks_after\":%" PRIu64,
            result->free_blocks_before, result->free_blocks_after);
  else
    fputs(",\"free_blocks_before\":null,\"free_blocks_after\":null",
          output);
  fputs(",\"validation_status\":", output);
  json_string(output, result->validation_ok ? "passed" : "failed");
  fprintf(output,
          ",\"error_code\":%d,\"has_durable_window\":%s",
          result->error_code,
          result->has_durable_window ? "true" : "false");
  if (result->has_reclaim_stats)
    fprintf(output,
            ",\"reclaim_attempts\":%" PRIu64
            ",\"reclaim_successes\":%" PRIu64
            ",\"reclaim_entries_purged\":%" PRIu64
            ",\"reclaim_blocks_reclaimed\":%" PRIu64
            ",\"reclaim_pre_commit_ns\":%" PRIu64
            ",\"reclaim_purge_ns\":%" PRIu64
            ",\"reclaim_post_commit_ns\":%" PRIu64
            ",\"reclaim_failures\":%" PRIu64,
            result->reclaim_attempts, result->reclaim_successes,
            result->reclaim_entries_purged, result->reclaim_blocks_reclaimed,
            result->reclaim_pre_commit_ns, result->reclaim_purge_ns,
            result->reclaim_post_commit_ns, result->reclaim_failures);
  else
    fputs(",\"reclaim_attempts\":null,\"reclaim_successes\":null,"
          "\"reclaim_entries_purged\":null,\"reclaim_blocks_reclaimed\":null,"
          "\"reclaim_pre_commit_ns\":null,\"reclaim_purge_ns\":null,"
          "\"reclaim_post_commit_ns\":null,\"reclaim_failures\":null",
          output);
  fputs(",\"error_message\":", output);
  json_string(output, result->error_message);
  fputs(",\"raw_latency_path\":", output);
  json_string(output, result->raw_latency_path);
  fputs("}\n", output);

  if (fclose(output) != 0)
    return -errno;
  return 0;
}
