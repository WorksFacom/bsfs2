#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BULK_REQUEST (1024U * 1024U)
#define RANDOM_REQUEST 4096U
#define PATTERN_BANKS 16U

struct bulk_parameters {
  uint64_t file_size;
  uint32_t random_reads;
  uint32_t random_writes;
};

static struct bulk_parameters bulk_parameters(
    const struct bench_run_config *config) {
  if (config->profile == BENCH_PROFILE_PUBLICATION)
    return (struct bulk_parameters){
        .file_size = 1024ULL * 1024ULL * 1024ULL,
        .random_reads = 20000,
        .random_writes = 10000,
    };
  return (struct bulk_parameters){
      .file_size = 16ULL * 1024ULL * 1024ULL,
      .random_reads = 256,
      .random_writes = 128,
  };
}

static int allocate_pattern_banks(const struct bench_run_config *config,
                                  unsigned char **out) {
  unsigned char *banks = malloc((size_t)PATTERN_BANKS * BULK_REQUEST);

  if (!banks)
    return -ENOMEM;
  for (uint64_t i = 0; i < PATTERN_BANKS; i++)
    bench_pattern_fill(banks + i * BULK_REQUEST, BULK_REQUEST, config->seed,
                       1, i * BULK_REQUEST);
  *out = banks;
  return 0;
}

static int write_bulk_file(struct bench_run_config *config,
                           const struct bulk_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_file file = {0};
  unsigned char *banks = NULL;
  uint64_t offset;
  int error;

  bench_result_init(&result, "bulk", "sequential-write");
  error = allocate_pattern_banks(config, &banks);
  if (error)
    goto emit_without_start;
  error = config->backend.ops->create_file(&config->backend, "/bulk.dat",
                                           &file);
  if (error)
    goto emit_without_start;

  bench_phase_start(config, &state);
  for (offset = 0; offset < parameters->file_size; offset += BULK_REQUEST) {
    size_t request =
        (size_t)((parameters->file_size - offset) < BULK_REQUEST
                     ? parameters->file_size - offset
                     : BULK_REQUEST);
    unsigned char *buffer =
        banks + ((offset / BULK_REQUEST) % PATTERN_BANKS) * BULK_REQUEST;

    error = bench_write_all(&config->backend, &file, buffer, request,
                            &result.operation_path_ns);
    if (error)
      break;
    result.operations++;
    result.logical_bytes += request;
  }
  (void)config->backend.ops->close_file(&config->backend, &file);
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  if (error)
    bench_result_error(&result, error, "bulk sequential write failed");
  free(banks);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }

emit_without_start:
  bench_phase_start(config, &state);
  bench_result_error(&result, error, "cannot prepare bulk file");
  free(banks);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int read_bulk_file(struct bench_run_config *config,
                          const struct bulk_parameters *parameters,
                          const char *phase) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_file file = {0};
  unsigned char *expected = NULL;
  unsigned char *buffer = NULL;
  uint64_t offset;
  int error;

  bench_result_init(&result, "bulk", phase);
  error = allocate_pattern_banks(config, &expected);
  if (error)
    goto emit;
  buffer = malloc(BULK_REQUEST);
  if (!buffer) {
    error = -ENOMEM;
    goto emit;
  }
  error =
      config->backend.ops->open_file(&config->backend, "/bulk.dat", 0, &file);
  if (error)
    goto emit;

  bench_phase_start(config, &state);
  for (offset = 0; offset < parameters->file_size; offset += BULK_REQUEST) {
    size_t request =
        (size_t)((parameters->file_size - offset) < BULK_REQUEST
                     ? parameters->file_size - offset
                     : BULK_REQUEST);
    unsigned char *want =
        expected + ((offset / BULK_REQUEST) % PATTERN_BANKS) * BULK_REQUEST;

    error = bench_read_all(&config->backend, &file, buffer, request,
                           &result.operation_path_ns);
    if (error)
      break;
    if (memcmp(buffer, want, request) != 0) {
      error = -EILSEQ;
      break;
    }
    result.operations++;
    result.logical_bytes += request;
  }
  result.elapsed_ns = result.operation_path_ns;
  (void)config->backend.ops->close_file(&config->backend, &file);
  if (error)
    bench_result_error(&result, error, "bulk sequential validation failed");
  free(expected);
  free(buffer);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }

emit:
  bench_phase_start(config, &state);
  bench_result_error(&result, error, "cannot prepare bulk read");
  if (file.private_data)
    (void)config->backend.ops->close_file(&config->backend, &file);
  free(expected);
  free(buffer);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static void random_offsets(uint32_t *offsets, uint32_t count,
                           uint64_t blocks, uint64_t seed, uint64_t stream) {
  uint64_t state = seed ^ stream;

  for (uint32_t i = 0; i < count; i++) {
    state = bench_splitmix64(state);
    offsets[i] = (uint32_t)(state % blocks);
  }
}

static int random_read_bulk(struct bench_run_config *config,
                            const struct bulk_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct bench_file file = {0};
  uint32_t *offsets = NULL;
  unsigned char buffer[RANDOM_REQUEST];
  unsigned char expected[RANDOM_REQUEST];
  char raw_path[BENCH_PATH_MAX] = {0};
  uint64_t blocks = parameters->file_size / RANDOM_REQUEST;
  int error;

  bench_result_init(&result, "bulk", "random-read-4k");
  error = bench_latency_init(&latencies, parameters->random_reads);
  if (error)
    goto emit_unstarted;
  offsets = malloc((size_t)parameters->random_reads * sizeof(*offsets));
  if (!offsets) {
    error = -ENOMEM;
    goto emit_unstarted;
  }
  random_offsets(offsets, parameters->random_reads, blocks, config->seed,
                 0x72656164ULL);
  error =
      config->backend.ops->open_file(&config->backend, "/bulk.dat", 0, &file);
  if (error)
    goto emit_unstarted;

  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->random_reads; i++) {
    uint64_t offset = (uint64_t)offsets[i] * RANDOM_REQUEST;
    uint64_t started;
    uint64_t elapsed;

    error = config->backend.ops->seek_file(&config->backend, &file, offset);
    if (error)
      break;
    started = bench_now_ns();
    error = bench_read_all(&config->backend, &file, buffer, sizeof(buffer),
                           NULL);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    bench_pattern_fill(expected, sizeof(expected), config->seed, 1,
                       (offset % (PATTERN_BANKS * BULK_REQUEST)));
    if (memcmp(buffer, expected, sizeof(buffer)) != 0) {
      error = -EILSEQ;
      break;
    }
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operations++;
    result.logical_bytes += sizeof(buffer);
    result.operation_path_ns += elapsed;
  }
  result.elapsed_ns = result.operation_path_ns;
  (void)config->backend.ops->close_file(&config->backend, &file);
  bench_latency_summarize(&latencies, &result);
  if (bench_latency_write_raw(config, result.phase, &latencies, raw_path,
                              sizeof(raw_path)) == 0 &&
      raw_path[0])
    result.raw_latency_path = raw_path;
  if (error)
    bench_result_error(&result, error, "bulk random read failed");
  free(offsets);
  {
    int operation_error = error;
    int output_error = bench_phase_finish(config, &state, &result);
    error = operation_error ? operation_error : output_error;
  }
  bench_latency_destroy(&latencies);
  return error;

emit_unstarted:
  bench_phase_start(config, &state);
  bench_result_error(&result, error, "cannot prepare random reads");
  free(offsets);
  bench_latency_destroy(&latencies);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int random_write_bulk(struct bench_run_config *config,
                             const struct bulk_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct bench_file file = {0};
  uint32_t *offsets = NULL;
  unsigned char *buffers = NULL;
  char raw_path[BENCH_PATH_MAX] = {0};
  uint64_t blocks = parameters->file_size / RANDOM_REQUEST;
  int error;

  bench_result_init(&result, "bulk", "random-overwrite-4k");
  error = bench_latency_init(&latencies, parameters->random_writes);
  if (error)
    goto emit_unstarted;
  offsets = malloc((size_t)parameters->random_writes * sizeof(*offsets));
  buffers = malloc((size_t)parameters->random_writes * RANDOM_REQUEST);
  if (!offsets || !buffers) {
    error = -ENOMEM;
    goto emit_unstarted;
  }
  random_offsets(offsets, parameters->random_writes, blocks, config->seed,
                 0x7772697465ULL);
  for (uint32_t i = 0; i < parameters->random_writes; i++)
    bench_pattern_fill(buffers + (size_t)i * RANDOM_REQUEST, RANDOM_REQUEST,
                       config->seed, 0x10000ULL + i, 0);
  error =
      config->backend.ops->open_file(&config->backend, "/bulk.dat", 1, &file);
  if (error)
    goto emit_unstarted;

  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->random_writes; i++) {
    uint64_t offset = (uint64_t)offsets[i] * RANDOM_REQUEST;
    uint64_t started;
    uint64_t elapsed;

    error = config->backend.ops->seek_file(&config->backend, &file, offset);
    if (error)
      break;
    started = bench_now_ns();
    error = bench_write_all(&config->backend, &file,
                            buffers + (size_t)i * RANDOM_REQUEST,
                            RANDOM_REQUEST, NULL);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operations++;
    result.logical_bytes += RANDOM_REQUEST;
    result.operation_path_ns += elapsed;
  }
  (void)config->backend.ops->close_file(&config->backend, &file);
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  bench_latency_summarize(&latencies, &result);
  if (bench_latency_write_raw(config, result.phase, &latencies, raw_path,
                              sizeof(raw_path)) == 0 &&
      raw_path[0])
    result.raw_latency_path = raw_path;
  if (error)
    bench_result_error(&result, error, "bulk random overwrite failed");
  free(offsets);
  free(buffers);
  {
    int operation_error = error;
    int output_error = bench_phase_finish(config, &state, &result);
    error = operation_error ? operation_error : output_error;
  }
  bench_latency_destroy(&latencies);
  return error;

emit_unstarted:
  bench_phase_start(config, &state);
  bench_result_error(&result, error, "cannot prepare random overwrites");
  if (file.private_data)
    (void)config->backend.ops->close_file(&config->backend, &file);
  free(offsets);
  free(buffers);
  bench_latency_destroy(&latencies);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

static int verify_random_writes(struct bench_run_config *config,
                                const struct bulk_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_file file = {0};
  uint32_t *offsets = NULL;
  int32_t *latest = NULL;
  unsigned char buffer[RANDOM_REQUEST];
  unsigned char expected[RANDOM_REQUEST];
  uint64_t blocks = parameters->file_size / RANDOM_REQUEST;
  int error = 0;

  bench_result_init(&result, "bulk", "verify-overwrites");
  offsets = malloc((size_t)parameters->random_writes * sizeof(*offsets));
  latest = malloc((size_t)blocks * sizeof(*latest));
  if (!offsets || !latest) {
    error = -ENOMEM;
    goto emit;
  }
  for (uint64_t i = 0; i < blocks; i++)
    latest[i] = -1;
  random_offsets(offsets, parameters->random_writes, blocks, config->seed,
                 0x7772697465ULL);
  for (uint32_t i = 0; i < parameters->random_writes; i++)
    latest[offsets[i]] = (int32_t)i;
  error =
      config->backend.ops->open_file(&config->backend, "/bulk.dat", 0, &file);
  if (error)
    goto emit;

  bench_phase_start(config, &state);
  for (uint64_t block = 0; block < blocks; block++) {
    uint64_t started;
    if (latest[block] < 0)
      continue;
    error = config->backend.ops->seek_file(&config->backend, &file,
                                           block * RANDOM_REQUEST);
    if (error)
      break;
    started = bench_now_ns();
    error = bench_read_all(&config->backend, &file, buffer, sizeof(buffer),
                           NULL);
    result.operation_path_ns += bench_now_ns() - started;
    if (error)
      break;
    bench_pattern_fill(expected, sizeof(expected), config->seed,
                       0x10000ULL + (uint32_t)latest[block], 0);
    if (memcmp(buffer, expected, sizeof(buffer)) != 0) {
      error = -EILSEQ;
      break;
    }
    result.operations++;
    result.logical_bytes += sizeof(buffer);
  }
  result.elapsed_ns = result.operation_path_ns;
  (void)config->backend.ops->close_file(&config->backend, &file);
  if (error)
    bench_result_error(&result, error, "bulk overwrite verification failed");
  free(offsets);
  free(latest);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }

emit:
  bench_phase_start(config, &state);
  bench_result_error(&result, error, "cannot prepare overwrite verification");
  free(offsets);
  free(latest);
  {
    int output_error = bench_phase_finish(config, &state, &result);
    return error ? error : output_error;
  }
}

int bench_run_bulk(struct bench_run_config *config) {
  struct bulk_parameters parameters = bulk_parameters(config);
  int error = 0;
  int result;

  if (bench_phase_selected(config, "prepare")) {
    result = write_bulk_file(config, &parameters);
    if (result && !error)
      error = result;
  }
  if (bench_phase_selected(config, "cold-read")) {
    result = read_bulk_file(config, &parameters, "sequential-read-cold");
    if (result && !error)
      error = result;
  }
  if (bench_phase_selected(config, "warm-read")) {
    result = read_bulk_file(config, &parameters, "sequential-read-warm");
    if (result && !error)
      error = result;
  }
  if (bench_phase_selected(config, "random-read")) {
    result = random_read_bulk(config, &parameters);
    if (result && !error)
      error = result;
  }
  if (bench_phase_selected(config, "random-write")) {
    result = random_write_bulk(config, &parameters);
    if (result && !error)
      error = result;
  }
  if (bench_phase_selected(config, "verify")) {
    result = verify_random_writes(config, &parameters);
    if (result && !error)
      error = result;
  }
  return error;
}
