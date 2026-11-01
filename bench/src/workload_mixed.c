#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIXED_CHUNK (1024U * 1024U)
#define EDIT_CHUNK 4096U

struct mixed_parameters {
  uint32_t directories;
  uint32_t files;
  uint32_t small_4k;
  uint32_t small_16k;
  uint32_t small_64k;
};

struct mixed_plan {
  uint32_t *order;
  unsigned char *append;
  unsigned char *overwrite;
  unsigned char *rename;
  unsigned char *remove;
};

static struct mixed_parameters mixed_parameters(
    const struct bench_run_config *config) {
  (void)config;
  return (struct mixed_parameters){
      .directories = 20,
      .files = 500,
      .small_4k = 250,
      .small_16k = 150,
      .small_64k = 75,
  };
}

static uint64_t mixed_file_size(const struct mixed_parameters *parameters,
                                uint32_t file) {
  if (file < parameters->small_4k)
    return 4096;
  if (file < parameters->small_4k + parameters->small_16k)
    return 16384;
  if (file < parameters->small_4k + parameters->small_16k +
                 parameters->small_64k)
    return 65536;
  return 1024 * 1024;
}

static int mixed_path(char *path, size_t capacity,
                      const struct mixed_parameters *parameters, uint32_t file,
                      int renamed) {
  return bench_format_path(path, capacity,
                           renamed ? "/mixed/d%03u/ren_%05u"
                                   : "/mixed/d%03u/file_%05u",
                           file % parameters->directories, file);
}

static int mixed_plan_create(const struct bench_run_config *config,
                             const struct mixed_parameters *parameters,
                             struct mixed_plan *plan) {
  uint32_t append_count = parameters->files / 5;
  uint32_t overwrite_count = parameters->files / 5;
  uint32_t rename_count = parameters->files / 10;
  uint32_t remove_count = parameters->files / 5;
  uint32_t cursor = 0;

  memset(plan, 0, sizeof(*plan));
  plan->order = malloc((size_t)parameters->files * sizeof(*plan->order));
  plan->append = calloc(parameters->files, 1);
  plan->overwrite = calloc(parameters->files, 1);
  plan->rename = calloc(parameters->files, 1);
  plan->remove = calloc(parameters->files, 1);
  if (!plan->order || !plan->append || !plan->overwrite || !plan->rename ||
      !plan->remove)
    return -ENOMEM;
  for (uint32_t i = 0; i < parameters->files; i++)
    plan->order[i] = i;
  bench_shuffle_u32(plan->order, parameters->files,
                    config->seed ^ 0x4d49584544ULL);
  for (uint32_t i = 0; i < append_count; i++)
    plan->append[plan->order[cursor++]] = 1;
  for (uint32_t i = 0; i < overwrite_count; i++)
    plan->overwrite[plan->order[cursor++]] = 1;
  for (uint32_t i = 0; i < rename_count; i++)
    plan->rename[plan->order[cursor++]] = 1;
  for (uint32_t i = 0; i < remove_count; i++)
    plan->remove[plan->order[cursor++]] = 1;
  return 0;
}

static void mixed_plan_destroy(struct mixed_plan *plan) {
  free(plan->order);
  free(plan->append);
  free(plan->overwrite);
  free(plan->rename);
  free(plan->remove);
  memset(plan, 0, sizeof(*plan));
}

static int emit_mixed_latency(struct bench_run_config *config,
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

static int mixed_create(struct bench_run_config *config,
                        const struct mixed_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  unsigned char *buffer;
  char path[BENCH_PATH_MAX];
  int error;

  error = config->backend.ops->mkdir(&config->backend, "/mixed");
  if (error)
    return error;
  for (uint32_t i = 0; i < parameters->directories; i++) {
    error = bench_format_path(path, sizeof(path), "/mixed/d%03u", i);
    if (!error)
      error = config->backend.ops->mkdir(&config->backend, path);
    if (error)
      return error;
  }

  buffer = malloc(MIXED_CHUNK);
  if (!buffer)
    return -ENOMEM;
  bench_result_init(&result, "mixed", "create-tree");
  error = bench_latency_init(&latencies, parameters->files);
  if (error) {
    free(buffer);
    return error;
  }
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->files; i++) {
    struct bench_file file = {0};
    uint64_t size = mixed_file_size(parameters, i);
    uint64_t operation_started;
    uint64_t operation_elapsed = 0;

    error = mixed_path(path, sizeof(path), parameters, i, 0);
    if (error)
      break;
    operation_started = bench_now_ns();
    error =
        config->backend.ops->create_file(&config->backend, path, &file);
    operation_elapsed += bench_now_ns() - operation_started;
    if (error)
      break;
    for (uint64_t offset = 0; offset < size; offset += MIXED_CHUNK) {
      size_t request =
          (size_t)((size - offset) < MIXED_CHUNK ? size - offset : MIXED_CHUNK);
      bench_pattern_fill(buffer, request, config->seed, 0x200000ULL + i,
                         offset);
      error = bench_write_all(&config->backend, &file, buffer, request,
                              &operation_elapsed);
      if (error)
        break;
    }
    operation_started = bench_now_ns();
    if (config->backend.ops->close_file(&config->backend, &file) && !error)
      error = -EIO;
    operation_elapsed += bench_now_ns() - operation_started;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, operation_elapsed)) != 0)
      break;
    result.operation_path_ns += operation_elapsed;
    result.operations++;
    result.logical_bytes += size;
  }
  if (!error)
    error = bench_phase_flush(config, &result);
  free(buffer);
  return emit_mixed_latency(config, &state, &result, &latencies, error,
                            "mixed tree creation failed");
}

static int mixed_read(struct bench_run_config *config,
                      const struct mixed_parameters *parameters) {
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  uint32_t *order;
  unsigned char *buffer;
  unsigned char *expected;
  char path[BENCH_PATH_MAX];
  int error;

  order = malloc((size_t)parameters->files * sizeof(*order));
  buffer = malloc(MIXED_CHUNK);
  expected = malloc(MIXED_CHUNK);
  if (!order || !buffer || !expected) {
    free(order);
    free(buffer);
    free(expected);
    return -ENOMEM;
  }
  for (uint32_t i = 0; i < parameters->files; i++)
    order[i] = i;
  bench_shuffle_u32(order, parameters->files, config->seed ^ 0x52454144ULL);

  bench_result_init(&result, "mixed", "cold-read-tree");
  error = bench_latency_init(&latencies, parameters->files);
  if (error)
    goto out;
  bench_phase_start(config, &state);
  for (uint32_t n = 0; n < parameters->files; n++) {
    struct bench_file file = {0};
    uint32_t i = order[n];
    uint64_t size = mixed_file_size(parameters, i);
    uint64_t operation_started;
    uint64_t operation_elapsed = 0;

    error = mixed_path(path, sizeof(path), parameters, i, 0);
    if (error)
      break;
    operation_started = bench_now_ns();
    error = config->backend.ops->open_file(&config->backend, path, 0, &file);
    operation_elapsed += bench_now_ns() - operation_started;
    if (error)
      break;
    for (uint64_t offset = 0; offset < size; offset += MIXED_CHUNK) {
      size_t request =
          (size_t)((size - offset) < MIXED_CHUNK ? size - offset : MIXED_CHUNK);
      error = bench_read_all(&config->backend, &file, buffer, request,
                             &operation_elapsed);
      if (error)
        break;
      bench_pattern_fill(expected, request, config->seed, 0x200000ULL + i,
                         offset);
      if (memcmp(buffer, expected, request) != 0) {
        error = -EILSEQ;
        break;
      }
    }
    operation_started = bench_now_ns();
    (void)config->backend.ops->close_file(&config->backend, &file);
    operation_elapsed += bench_now_ns() - operation_started;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, operation_elapsed)) != 0)
      break;
    result.operation_path_ns += operation_elapsed;
    result.operations++;
    result.logical_bytes += size;
  }
  free(order);
  free(buffer);
  free(expected);
  return emit_mixed_latency(config, &state, &result, &latencies, error,
                            "mixed tree read validation failed");

out:
  free(order);
  free(buffer);
  free(expected);
  return error;
}

static int emit_edit_operation(struct bench_run_config *config,
                               const struct mixed_parameters *parameters,
                               const struct mixed_plan *plan, int operation,
                               uint64_t *cycle_operation_ns,
                               uint64_t *cycle_operations,
                               uint64_t *cycle_bytes) {
  static const char *const phases[] = {
      "append-4k", "overwrite-4k", "rename", "native-namespace-removal"};
  static const char *const messages[] = {
      "mixed append failed", "mixed overwrite failed", "mixed rename failed",
      "mixed native namespace removal failed"};
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  const unsigned char *selected;
  unsigned char buffer[EDIT_CHUNK];
  char old_path[BENCH_PATH_MAX];
  char new_path[BENCH_PATH_MAX];
  int error;

  selected = operation == 0   ? plan->append
             : operation == 1 ? plan->overwrite
             : operation == 2 ? plan->rename
                              : plan->remove;
  bench_result_init(&result, "mixed", phases[operation]);
  error = bench_latency_init(&latencies, parameters->files / 5);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->files; i++) {
    struct bench_file file = {0};
    uint64_t elapsed;
    uint64_t start;

    if (!selected[i])
      continue;
    error = mixed_path(old_path, sizeof(old_path), parameters, i, 0);
    if (error)
      break;
    if (operation == 0 || operation == 1)
      bench_pattern_fill(buffer, sizeof(buffer), config->seed,
                         (operation == 0 ? 0x300000ULL : 0x400000ULL) + i, 0);
    start = bench_now_ns();
    if (operation == 0 || operation == 1) {
      uint64_t size = mixed_file_size(parameters, i);
      uint64_t offset =
          operation == 0 ? size : ((uint64_t)i * EDIT_CHUNK) % size;
      error =
          config->backend.ops->open_file(&config->backend, old_path, 1, &file);
      if (!error)
        error =
            config->backend.ops->seek_file(&config->backend, &file, offset);
      if (!error)
        error = bench_write_all(&config->backend, &file, buffer,
                                sizeof(buffer), NULL);
      if (file.private_data)
        (void)config->backend.ops->close_file(&config->backend, &file);
      if (!error)
        result.logical_bytes += sizeof(buffer);
    } else if (operation == 2) {
      error = mixed_path(new_path, sizeof(new_path), parameters, i, 1);
      if (!error)
        error =
            config->backend.ops->rename(&config->backend, old_path, new_path);
    } else {
      error = config->backend.ops->unlink(&config->backend, old_path);
    }
    elapsed = bench_now_ns() - start;
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
  }
  *cycle_operation_ns += result.operation_path_ns;
  *cycle_operations += result.operations;
  *cycle_bytes += result.logical_bytes;
  return emit_mixed_latency(config, &state, &result, &latencies, error,
                            messages[operation]);
}

static int mixed_edit(struct bench_run_config *config,
                      const struct mixed_parameters *parameters) {
  struct mixed_plan plan;
  struct bench_phase_state state;
  struct bench_result result;
  uint64_t cycle_operation_ns = 0;
  uint64_t cycle_operations = 0;
  uint64_t cycle_bytes = 0;
  int error;

  error = mixed_plan_create(config, parameters, &plan);
  if (error)
    return error;
  bench_phase_start(config, &state);
  for (int operation = 0; operation < 4; operation++) {
    error = emit_edit_operation(config, parameters, &plan, operation,
                                &cycle_operation_ns, &cycle_operations,
                                &cycle_bytes);
    if (error)
      break;
  }
  bench_result_init(&result, "mixed", "edit-cycle-durable-total");
  result.operation_path_ns = cycle_operation_ns;
  result.operations = cycle_operations;
  result.logical_bytes = cycle_bytes;
  if (!error)
    error = bench_phase_flush(config, &result);
  result.elapsed_ns = result.operation_path_ns;
  if (error)
    bench_result_error(&result, error, "mixed edit cycle failed");
  {
    int output_error = bench_phase_finish(config, &state, &result);
    mixed_plan_destroy(&plan);
    return error ? error : output_error;
  }
}

static int validate_mixed_file(struct bench_run_config *config,
                               const struct mixed_parameters *parameters,
                               const struct mixed_plan *plan, uint32_t file_id,
                               unsigned char *buffer, unsigned char *expected,
                               uint64_t *elapsed) {
  struct bench_file file = {0};
  struct bench_stat status;
  char path[BENCH_PATH_MAX];
  uint64_t original_size = mixed_file_size(parameters, file_id);
  uint64_t final_size = original_size + (plan->append[file_id] ? EDIT_CHUNK : 0);
  uint64_t started;
  int error;

  error = mixed_path(path, sizeof(path), parameters, file_id,
                     plan->rename[file_id]);
  if (error)
    return error;
  started = bench_now_ns();
  error = config->backend.ops->stat(&config->backend, path, &status);
  *elapsed += bench_now_ns() - started;
  if (plan->remove[file_id])
    return error == -ENOENT ? 0 : (error ? error : -EEXIST);
  if (error)
    return error;
  if (status.size != final_size)
    return -EILSEQ;
  started = bench_now_ns();
  error =
      config->backend.ops->open_file(&config->backend, path, 0, &file);
  *elapsed += bench_now_ns() - started;
  if (error)
    return error;
  for (uint64_t offset = 0; offset < final_size; offset += MIXED_CHUNK) {
    size_t request =
        (size_t)((final_size - offset) < MIXED_CHUNK ? final_size - offset
                                                     : MIXED_CHUNK);
    error =
        bench_read_all(&config->backend, &file, buffer, request, elapsed);
    if (error)
      break;
    if (offset < original_size) {
      size_t initial =
          (size_t)((original_size - offset) < request ? original_size - offset
                                                     : request);
      bench_pattern_fill(expected, initial, config->seed,
                         0x200000ULL + file_id, offset);
      if (plan->overwrite[file_id]) {
        uint64_t overwrite_offset =
            ((uint64_t)file_id * EDIT_CHUNK) % original_size;
        uint64_t chunk_end = offset + initial;
        uint64_t overwrite_end = overwrite_offset + EDIT_CHUNK;
        if (overwrite_offset < chunk_end && overwrite_end > offset) {
          uint64_t begin =
              overwrite_offset > offset ? overwrite_offset - offset : 0;
          uint64_t source_offset =
              offset > overwrite_offset ? offset - overwrite_offset : 0;
          uint64_t end =
              overwrite_end < chunk_end ? overwrite_end - offset : initial;
          bench_pattern_fill(expected + begin, (size_t)(end - begin),
                             config->seed, 0x400000ULL + file_id,
                             source_offset);
        }
      }
      if (memcmp(buffer, expected, initial) != 0) {
        error = -EILSEQ;
        break;
      }
      if (request > initial) {
        bench_pattern_fill(expected, request - initial, config->seed,
                           0x300000ULL + file_id, 0);
        if (memcmp(buffer + initial, expected, request - initial) != 0) {
          error = -EILSEQ;
          break;
        }
      }
    } else {
      bench_pattern_fill(expected, request, config->seed,
                         0x300000ULL + file_id, offset - original_size);
      if (memcmp(buffer, expected, request) != 0) {
        error = -EILSEQ;
        break;
      }
    }
  }
  started = bench_now_ns();
  (void)config->backend.ops->close_file(&config->backend, &file);
  *elapsed += bench_now_ns() - started;
  return error;
}

static int mixed_verify(struct bench_run_config *config,
                        const struct mixed_parameters *parameters) {
  struct mixed_plan plan;
  struct bench_phase_state state;
  struct bench_result result;
  struct bench_latency latencies;
  unsigned char *buffer;
  unsigned char *expected;
  int error;

  error = mixed_plan_create(config, parameters, &plan);
  if (error)
    return error;
  buffer = malloc(MIXED_CHUNK);
  expected = malloc(MIXED_CHUNK);
  if (!buffer || !expected) {
    free(buffer);
    free(expected);
    mixed_plan_destroy(&plan);
    return -ENOMEM;
  }
  bench_result_init(&result, "mixed", "verify-tree");
  error = bench_latency_init(&latencies, parameters->files);
  if (error)
    goto out;
  bench_phase_start(config, &state);
  for (uint32_t i = 0; i < parameters->files; i++) {
    uint64_t elapsed = 0;

    error = validate_mixed_file(config, parameters, &plan, i, buffer, expected,
                                &elapsed);
    if (error)
      break;
    if ((error = bench_latency_add(&latencies, elapsed)) != 0)
      break;
    result.operation_path_ns += elapsed;
    result.operations++;
    if (!plan.remove[i])
      result.logical_bytes +=
          mixed_file_size(parameters, i) + (plan.append[i] ? EDIT_CHUNK : 0);
  }
  free(buffer);
  free(expected);
  mixed_plan_destroy(&plan);
  return emit_mixed_latency(config, &state, &result, &latencies, error,
                            "mixed final validation failed");

out:
  free(buffer);
  free(expected);
  mixed_plan_destroy(&plan);
  return error;
}

int bench_run_mixed(struct bench_run_config *config) {
  struct mixed_parameters parameters = mixed_parameters(config);
  int error;

  if (bench_phase_selected(config, "create") &&
      (error = mixed_create(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "read") &&
      (error = mixed_read(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "edit") &&
      (error = mixed_edit(config, &parameters)) != 0)
    return error;
  if (bench_phase_selected(config, "verify") &&
      (error = mixed_verify(config, &parameters)) != 0)
    return error;
  return 0;
}
