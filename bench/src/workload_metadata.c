#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct metadata_parameters {
  uint32_t directories;
  uint32_t files;
  uint32_t renames;
};

struct iteration_context {
  unsigned char *seen;
  uint32_t files;
  uint32_t directories;
  uint32_t directory;
  uint32_t count;
};

static struct metadata_parameters metadata_parameters(
    const struct bench_run_config *config) {
  (void)config;
  return (struct metadata_parameters){
      .directories = 20,
      .files = 1000,
      .renames = 200,
  };
}

static int metadata_path(char *path, size_t capacity, uint32_t directory,
                         uint32_t file, int renamed) {
  if (renamed)
    return bench_format_path(path, capacity, "/metadata/d%03u/r%05u",
                             directory, file);
  return bench_format_path(path, capacity, "/metadata/d%03u/f%05u",
                           directory, file);
}

static int finish_latency_phase(struct bench_run_config *config,
                                struct bench_phase_state *state,
                                struct bench_result *result,
                                struct bench_latency *latencies, int error,
                                const char *message) {
  char raw_path[BENCH_PATH_MAX] = {0};
  int output_error;

  result->elapsed_ns = result->operation_path_ns;
  bench_latency_summarize(latencies, result);
  if (bench_latency_write_raw(config, result->phase, latencies, raw_path,
                              sizeof(raw_path)) == 0 &&
      raw_path[0])
    result->raw_latency_path = raw_path;
  if (error)
    bench_result_error(result, error, message);
  output_error = bench_phase_finish(config, state, result);
  bench_latency_destroy(latencies);
  return error ? error : output_error;
}

static int metadata_create(struct bench_run_config *config,
                           const struct metadata_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  char path[BENCH_PATH_MAX];
  int error;

  bench_result_init(&result, "metadata", "mkdir");
  error = bench_latency_init(&latencies, parameters->directories + 1);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i <= parameters->directories; i++) {
    uint64_t start;
    uint64_t elapsed;

    if (i == 0)
      error = bench_format_path(path, sizeof(path), "/metadata");
    else
      error = bench_format_path(path, sizeof(path), "/metadata/d%03u", i - 1);
    if (error)
      break;
    start = bench_now_ns();
    error = config->backend.ops->mkdir(&config->backend, path);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  error = finish_latency_phase(config, &state, &result, &latencies, error,
                               "metadata directory creation failed");
  if (error)
    return error;

  bench_result_init(&result, "metadata", "create");
  error = bench_latency_init(&latencies, parameters->files);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->files; i++) {
    struct bench_file file = {0};
    uint64_t start;
    uint64_t elapsed;

    error = metadata_path(path, sizeof(path), i % parameters->directories, i,
                          0);
    if (error)
      break;
    start = bench_now_ns();
    error =
        config->backend.ops->create_file(&config->backend, path, &file);
    if (!error)
      error = config->backend.ops->close_file(&config->backend, &file);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  return finish_latency_phase(config, &state, &result, &latencies, error,
                              "metadata file creation failed");
}

/*
 * runs cold (after orchestration drops caches) then warm. warm only measures the shared
 * dentry cache, so only the cold pass says anything about on-disk layout
 */
static int metadata_stat(struct bench_run_config *config,
                         const struct metadata_parameters *parameters,
                         const char *phase) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct bench_stat status;
  uint32_t *order;
  char path[BENCH_PATH_MAX];
  int error;

  order = malloc((size_t)parameters->files * sizeof(*order));
  if (!order)
    return -ENOMEM;
  for (uint32_t i = 0; i < parameters->files; i++)
    order[i] = i;
  bench_shuffle_u32(order, parameters->files, config->seed ^ 0x53544154ULL);
  bench_result_init(&result, "metadata", phase);
  error = bench_latency_init(&latencies, parameters->files);
  if (error) {
    free(order);
    return error;
  }
  bench_phase_start(config, &state);
  for (uint32_t n = 0; n < parameters->files; n++) {
    uint32_t i = order[n];
    uint64_t start;
    uint64_t elapsed;

    error = metadata_path(path, sizeof(path), i % parameters->directories, i,
                          0);
    if (error)
      break;
    start = bench_now_ns();
    error = config->backend.ops->stat(&config->backend, path, &status);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if (status.size != 0) {
      error = -EILSEQ;
      break;
    }
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  free(order);
  return finish_latency_phase(config, &state, &result, &latencies, error,
                              "metadata stat validation failed");
}

static int iteration_callback(const char *name, void *opaque) {
  struct iteration_context *context = opaque;
  char *end;
  unsigned long value;

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return 0;
  if (name[0] != 'f')
    return -EILSEQ;
  errno = 0;
  value = strtoul(name + 1, &end, 10);
  if (errno || !end || *end || value >= context->files)
    return -EILSEQ;
  if ((uint32_t)value % context->directories != context->directory)
    return -EILSEQ;
  if (context->seen[value])
    return -EEXIST;
  context->seen[value] = 1;
  context->count++;
  return 0;
}

static int metadata_iterate(struct bench_run_config *config,
                            const struct metadata_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  struct iteration_context context = {
      .files = parameters->files,
      .directories = parameters->directories,
  };
  char path[BENCH_PATH_MAX];
  int error;

  context.seen = calloc(parameters->files, 1);
  if (!context.seen)
    return -ENOMEM;
  bench_result_init(&result, "metadata", "iteration");
  error = bench_latency_init(&latencies, parameters->directories);
  if (error) {
    free(context.seen);
    return error;
  }
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->directories; i++) {
    uint64_t start;
    uint64_t elapsed;

    context.directory = i;
    error = bench_format_path(path, sizeof(path), "/metadata/d%03u", i);
    if (error)
      break;
    start = bench_now_ns();
    error = config->backend.ops->iterate(&config->backend, path,
                                         iteration_callback, &context);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  if (!error && context.count != parameters->files)
    error = -EILSEQ;
  free(context.seen);
  return finish_latency_phase(config, &state, &result, &latencies, error,
                              "metadata directory iteration failed");
}

static int metadata_rename(struct bench_run_config *config,
                           const struct metadata_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  char old_path[BENCH_PATH_MAX];
  char new_path[BENCH_PATH_MAX];
  int error;

  bench_result_init(&result, "metadata", "rename");
  error = bench_latency_init(&latencies, parameters->renames);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->renames; i++) {
    uint32_t source = i % parameters->directories;
    uint32_t destination = (source + 1) % parameters->directories;
    uint64_t start;
    uint64_t elapsed;

    error = metadata_path(old_path, sizeof(old_path), source, i, 0);
    if (!error)
      error = metadata_path(new_path, sizeof(new_path), destination, i, 1);
    if (error)
      break;
    start = bench_now_ns();
    error = config->backend.ops->rename(&config->backend, old_path, new_path);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  return finish_latency_phase(config, &state, &result, &latencies, error,
                              "metadata rename failed");
}

static int metadata_unlink(struct bench_run_config *config,
                           const struct metadata_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  char path[BENCH_PATH_MAX];
  int error;

  bench_result_init(&result, "metadata", "native-namespace-removal");
  error = bench_latency_init(&latencies, parameters->files);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->files; i++) {
    uint32_t directory =
        i < parameters->renames
            ? ((i % parameters->directories) + 1) % parameters->directories
            : i % parameters->directories;
    uint64_t start;
    uint64_t elapsed;

    error = metadata_path(path, sizeof(path), directory, i,
                          i < parameters->renames);
    if (error)
      break;
    start = bench_now_ns();
    error = config->backend.ops->unlink(&config->backend, path);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  return finish_latency_phase(config, &state, &result, &latencies, error,
                              "metadata native namespace removal failed");
}

static int metadata_rmdir(struct bench_run_config *config,
                          const struct metadata_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  char path[BENCH_PATH_MAX];
  int error;

  bench_result_init(&result, "metadata", "directory-removal");
  error = bench_latency_init(&latencies, parameters->directories + 1);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i <= parameters->directories; i++) {
    uint64_t start;
    uint64_t elapsed;

    if (i < parameters->directories)
      error = bench_format_path(path, sizeof(path), "/metadata/d%03u", i);
    else
      error = bench_format_path(path, sizeof(path), "/metadata");
    if (error)
      break;
    start = bench_now_ns();
    error = config->backend.ops->rmdir(&config->backend, path);
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  return finish_latency_phase(config, &state, &result, &latencies, error,
                              "metadata directory removal failed");
}

static int metadata_verify(struct bench_run_config *config) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_stat status;
  int error;

  bench_result_init(&result, "metadata", "verify-empty-namespace");
  bench_phase_start(config, &state);
  error = config->backend.ops->stat(&config->backend, "/metadata", &status);
  if (error == -ENOENT)
    error = 0;
  else if (error == 0)
    error = -EEXIST;
  result.operations = 1;
  if (error)
    bench_result_error(&result, error, "metadata namespace is not empty");
  return bench_phase_finish(config, &state, &result);
}

int bench_run_metadata(struct bench_run_config *config) {
  struct metadata_parameters parameters = metadata_parameters(config);
  int error;

  if (bench_phase_selected(config, "create") &&
      (error = metadata_create(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "stat-cold") &&
      (error = metadata_stat(config, &parameters, "lookup-stat-cold")) != 0)
    return error;
  if (bench_phase_selected(config, "stat-warm") &&
      (error = metadata_stat(config, &parameters, "lookup-stat-warm")) != 0)
    return error;
  if (bench_phase_selected(config, "iterate") &&
      (error = metadata_iterate(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "rename") &&
      (error = metadata_rename(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "unlink") &&
      (error = metadata_unlink(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "rmdir") &&
      (error = metadata_rmdir(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "verify") &&
      (error = metadata_verify(config)) != 0)
    return error;
  return 0;
}
