#define _GNU_SOURCE
#include "workload_internal.h"
#include "bench_util.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONCURRENT_REQUEST (1024U * 1024U)
#define CONCURRENT_BANKS 16U

struct worker_result {
  struct bench_run_config *config;
  struct start_gate *gate;
  unsigned int worker;
  const char *phase;
  uint64_t operations;
  uint64_t logical_bytes;
  uint64_t elapsed_ns;
  struct bench_latency latencies;
  int error;
};

struct start_gate {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  unsigned int ready;
  int released;
  int aborted;
};

static int pin_worker_cpu(const struct worker_result *worker) {
  cpu_set_t set;
  int error;

  if (worker->config->cpu_count == 0)
    return 0;
  if (worker->worker >= worker->config->cpu_count)
    return -EINVAL;
  CPU_ZERO(&set);
  CPU_SET(worker->config->cpus[worker->worker], &set);
  error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  return error ? -error : 0;
}

static int start_gate_init(struct start_gate *gate) {
  int error;

  memset(gate, 0, sizeof(*gate));
  error = pthread_mutex_init(&gate->mutex, NULL);
  if (error)
    return -error;
  error = pthread_cond_init(&gate->condition, NULL);
  if (error) {
    pthread_mutex_destroy(&gate->mutex);
    return -error;
  }
  return 0;
}

static void start_gate_destroy(struct start_gate *gate) {
  pthread_cond_destroy(&gate->condition);
  pthread_mutex_destroy(&gate->mutex);
}

static int start_gate_wait(struct start_gate *gate) {
  int aborted;

  pthread_mutex_lock(&gate->mutex);
  gate->ready++;
  pthread_cond_broadcast(&gate->condition);
  while (!gate->released)
    pthread_cond_wait(&gate->condition, &gate->mutex);
  aborted = gate->aborted;
  pthread_mutex_unlock(&gate->mutex);
  return aborted ? -ECANCELED : 0;
}

static void start_gate_await_ready(struct start_gate *gate,
                                   unsigned int expected) {
  pthread_mutex_lock(&gate->mutex);
  while (gate->ready < expected)
    pthread_cond_wait(&gate->condition, &gate->mutex);
  pthread_mutex_unlock(&gate->mutex);
}

static void start_gate_release(struct start_gate *gate, int aborted) {
  pthread_mutex_lock(&gate->mutex);
  gate->aborted = aborted;
  gate->released = 1;
  pthread_cond_broadcast(&gate->condition);
  pthread_mutex_unlock(&gate->mutex);
}

static uint64_t concurrency_size(const struct bench_run_config *config) {
  (void)config;
  return 32ULL * 1024ULL * 1024ULL;
}

static void *sequential_worker(void *opaque) {
  struct worker_result *worker = opaque;
  struct bench_backend *backend = &worker->config->backend;
  struct bench_file file = {0};
  unsigned char *banks = NULL;
  unsigned char *scratch = NULL;
  char path[BENCH_PATH_MAX];
  uint64_t total = concurrency_size(worker->config);
  uint64_t wall_start;

  worker->error = pin_worker_cpu(worker);
  if (worker->error)
    goto ready;
  /* distinct content per offset bank, so the file is not trivially dedup-able */
  banks = malloc((size_t)CONCURRENT_BANKS * CONCURRENT_REQUEST);
  scratch = malloc(CONCURRENT_REQUEST);
  if (!banks || !scratch) {
    worker->error = -ENOMEM;
    goto ready;
  }
  for (uint64_t bank = 0; bank < CONCURRENT_BANKS; bank++)
    bench_pattern_fill(banks + bank * CONCURRENT_REQUEST, CONCURRENT_REQUEST,
                       worker->config->seed, 0x700000ULL + worker->worker,
                       bank * CONCURRENT_REQUEST);
  worker->error =
      bench_latency_init(&worker->latencies, total / CONCURRENT_REQUEST);
  if (worker->error)
    goto ready;
  worker->error = bench_format_path(
      path, sizeof(path), "/concurrency/w%u/sequential.dat", worker->worker);
  if (worker->error)
    goto ready;
  if (strcmp(worker->phase, "write") == 0)
    worker->error = backend->ops->create_file(backend, path, &file);
  else
    worker->error = backend->ops->open_file(backend, path, 0, &file);

ready:
  if (start_gate_wait(worker->gate) != 0)
    worker->error = -ECANCELED;
  wall_start = bench_now_ns();
  if (worker->error)
    goto out;
  for (uint64_t offset = 0; offset < total; offset += CONCURRENT_REQUEST) {
    const unsigned char *bank =
        banks +
        ((offset / CONCURRENT_REQUEST) % CONCURRENT_BANKS) * CONCURRENT_REQUEST;
    uint64_t started = bench_now_ns();
    uint64_t elapsed;

    if (strcmp(worker->phase, "write") == 0)
      worker->error =
          bench_write_all(backend, &file, bank, CONCURRENT_REQUEST, NULL);
    else
      worker->error =
          bench_read_all(backend, &file, scratch, CONCURRENT_REQUEST, NULL);
    elapsed = bench_now_ns() - started;
    if (worker->error)
      break;
    if (strcmp(worker->phase, "read") == 0 &&
        memcmp(scratch, bank, CONCURRENT_REQUEST) != 0) {
      worker->error = -EILSEQ;
      break;
    }
    worker->error = bench_latency_add(&worker->latencies, elapsed);
    if (worker->error)
      break;
    worker->operations++;
    worker->logical_bytes += CONCURRENT_REQUEST;
  }
out:
  worker->elapsed_ns = bench_now_ns() - wall_start;
  if (file.private_data)
    (void)backend->ops->close_file(backend, &file);
  free(banks);
  free(scratch);
  return NULL;
}

static int setup_sequential_directories(struct bench_run_config *config) {
  char path[BENCH_PATH_MAX];
  int error;

  error = config->backend.ops->mkdir(&config->backend, "/concurrency");
  if (error)
    return error;
  for (unsigned int i = 0; i < config->worker_count; i++) {
    error =
        bench_format_path(path, sizeof(path), "/concurrency/w%u", i);
    if (!error)
      error = config->backend.ops->mkdir(&config->backend, path);
    if (error)
      return error;
  }
  return 0;
}

static int emit_concurrency_results(struct bench_run_config *config,
                                    const char *aggregate_phase,
                                    struct worker_result *workers,
                                    struct bench_phase_state *state,
                                    int flush) {
  struct bench_result aggregate;
  struct bench_latency aggregate_latencies;
  uint64_t maximum_elapsed = 0;
  char phase[96];
  char raw_path[BENCH_PATH_MAX] = {0};
  int error = bench_latency_init(&aggregate_latencies, 0);

  if (error)
    return error;
  bench_result_init(&aggregate, config->scenario, aggregate_phase);
  for (unsigned int i = 0; i < config->worker_count; i++) {
    struct bench_result result;

    snprintf(phase, sizeof(phase), "%s-worker-%u", aggregate_phase, i);
    bench_result_init(&result, config->scenario, phase);
    result.has_worker_id = 1;
    result.worker_id = i;
    result.operations = workers[i].operations;
    result.logical_bytes = workers[i].logical_bytes;
    result.elapsed_ns = workers[i].elapsed_ns;
    result.operation_path_ns = workers[i].elapsed_ns;
    bench_latency_summarize(&workers[i].latencies, &result);
    if (workers[i].error)
      bench_result_error(&result, workers[i].error,
                         "concurrency worker failed");
    if (bench_result_write(config, &result) != 0 && !error)
      error = -EIO;
    aggregate.operations += workers[i].operations;
    aggregate.logical_bytes += workers[i].logical_bytes;
    if (workers[i].elapsed_ns > maximum_elapsed)
      maximum_elapsed = workers[i].elapsed_ns;
    for (size_t n = 0; n < workers[i].latencies.count; n++) {
      int add_error =
          bench_latency_add(&aggregate_latencies, workers[i].latencies.values[n]);
      if (add_error && !error)
        error = add_error;
    }
    if (workers[i].error && !error)
      error = workers[i].error;
  }
  aggregate.elapsed_ns = maximum_elapsed;
  aggregate.operation_path_ns = maximum_elapsed;
  if (flush) {
    /* flush even after a worker failure so the next phase reads settled state */
    int flush_error = bench_phase_flush(config, &aggregate);
    if (!error)
      error = flush_error;
  }
  bench_latency_summarize(&aggregate_latencies, &aggregate);
  if (bench_latency_write_raw(config, aggregate.phase, &aggregate_latencies,
                              raw_path, sizeof(raw_path)) == 0 &&
      raw_path[0])
    aggregate.raw_latency_path = raw_path;
  if (error)
    bench_result_error(&aggregate, error, "concurrency aggregate failed");
  {
    int output_error = bench_phase_finish(config, state, &aggregate);
    bench_latency_destroy(&aggregate_latencies);
    return error ? error : output_error;
  }
}

static int run_sequential_phase(struct bench_run_config *config,
                                const char *phase) {
  struct start_gate gate;
  pthread_t *threads;
  struct worker_result *workers;
  struct bench_phase_state state;
  int error = 0;
  unsigned int created = 0;

  if (strcmp(phase, "write") == 0) {
    error = setup_sequential_directories(config);
    if (error)
      return error;
  }
  threads = calloc(config->worker_count, sizeof(*threads));
  workers = calloc(config->worker_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -ENOMEM;
  }
  error = start_gate_init(&gate);
  if (error) {
    free(threads);
    free(workers);
    return error;
  }
  for (unsigned int i = 0; i < config->worker_count; i++) {
    workers[i].config = config;
    workers[i].gate = &gate;
    workers[i].worker = i;
    workers[i].phase = phase;
    if (pthread_create(&threads[i], NULL, sequential_worker, &workers[i]) != 0) {
      error = -EAGAIN;
      break;
    }
    created++;
  }
  if (created != config->worker_count) {
    start_gate_release(&gate, 1);
    for (unsigned int i = 0; i < created; i++)
      pthread_join(threads[i], NULL);
    goto out;
  }
  /* snapshot only once every worker has finished setup and is parked */
  start_gate_await_ready(&gate, created);
  bench_phase_start(config, &state);
  start_gate_release(&gate, 0);
  for (unsigned int i = 0; i < created; i++)
    pthread_join(threads[i], NULL);
  error = emit_concurrency_results(
      config,
      strcmp(phase, "write") == 0 ? "sequential-write"
                                   : "sequential-read-cold",
      workers, &state, strcmp(phase, "write") == 0);

out:
  for (unsigned int i = 0; i < created; i++)
    bench_latency_destroy(&workers[i].latencies);
  start_gate_destroy(&gate);
  free(threads);
  free(workers);
  return error;
}

struct concurrent_mixed_plan {
  unsigned char *append;
  unsigned char *overwrite;
  unsigned char *rename;
  unsigned char *remove;
};

static uint32_t concurrent_mixed_files(
    const struct bench_run_config *config) {
  (void)config;
  return 250;
}

static uint32_t concurrent_mixed_directories(
    const struct bench_run_config *config) {
  (void)config;
  return 20;
}

static uint64_t concurrent_mixed_size(
    const struct bench_run_config *config, uint32_t file) {
  (void)config;
  if (file < 125)
    return 4096;
  if (file < 200)
    return 16384;
  if (file < 238)
    return 65536;
  return 1024 * 1024;
}

static int concurrent_plan_create(const struct worker_result *worker,
                                  struct concurrent_mixed_plan *plan) {
  uint32_t files = concurrent_mixed_files(worker->config);
  uint32_t *order = malloc((size_t)files * sizeof(*order));
  uint32_t cursor = 0;

  memset(plan, 0, sizeof(*plan));
  plan->append = calloc(files, 1);
  plan->overwrite = calloc(files, 1);
  plan->rename = calloc(files, 1);
  plan->remove = calloc(files, 1);
  if (!order || !plan->append || !plan->overwrite || !plan->rename ||
      !plan->remove) {
    free(order);
    return -ENOMEM;
  }
  for (uint32_t i = 0; i < files; i++)
    order[i] = i;
  bench_shuffle_u32(order, files,
                    worker->config->seed ^ ((uint64_t)worker->worker << 32));
  for (uint32_t i = 0; i < files / 5; i++)
    plan->append[order[cursor++]] = 1;
  for (uint32_t i = 0; i < files / 5; i++)
    plan->overwrite[order[cursor++]] = 1;
  for (uint32_t i = 0; i < files / 10; i++)
    plan->rename[order[cursor++]] = 1;
  for (uint32_t i = 0; i < files / 5; i++)
    plan->remove[order[cursor++]] = 1;
  free(order);
  return 0;
}

static void concurrent_plan_destroy(struct concurrent_mixed_plan *plan) {
  free(plan->append);
  free(plan->overwrite);
  free(plan->rename);
  free(plan->remove);
}

static int concurrent_path(char *path, size_t capacity,
                           const struct worker_result *worker, uint32_t file,
                           int renamed) {
  uint32_t directories = concurrent_mixed_directories(worker->config);
  return bench_format_path(
      path, capacity,
      renamed ? "/concurrent-mixed/w%u/d%03u/ren_%05u"
              : "/concurrent-mixed/w%u/d%03u/file_%05u",
      worker->worker, file % directories, file);
}

static int mixed_worker_create(struct worker_result *worker,
                               unsigned char *buffer) {
  struct bench_backend *backend = &worker->config->backend;
  uint32_t files = concurrent_mixed_files(worker->config);
  char path[BENCH_PATH_MAX];
  int error = 0;

  for (uint32_t i = 0; i < files; i++) {
    struct bench_file file = {0};
    uint64_t size = concurrent_mixed_size(worker->config, i);
    uint64_t started;
    uint64_t elapsed;

    error = concurrent_path(path, sizeof(path), worker, i, 0);
    if (error)
      break;
    bench_pattern_fill(buffer, (size_t)size, worker->config->seed,
                       0x800000ULL + ((uint64_t)worker->worker << 32) + i, 0);
    started = bench_now_ns();
    error = backend->ops->create_file(backend, path, &file);
    if (!error)
      error = bench_write_all(backend, &file, buffer, (size_t)size, NULL);
    if (file.private_data)
      (void)backend->ops->close_file(backend, &file);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    if ((error = bench_latency_add(&worker->latencies, elapsed)) != 0)
      break;
    worker->operations++;
    worker->logical_bytes += size;
  }
  return error;
}

static int mixed_worker_read_or_verify(
    struct worker_result *worker, const struct concurrent_mixed_plan *plan,
    unsigned char *buffer, unsigned char *expected, int verify) {
  struct bench_backend *backend = &worker->config->backend;
  uint32_t files = concurrent_mixed_files(worker->config);
  char path[BENCH_PATH_MAX];
  int error = 0;

  for (uint32_t i = 0; i < files; i++) {
    struct bench_file file = {0};
    struct bench_stat status;
    uint64_t size = concurrent_mixed_size(worker->config, i);
    uint64_t final_size =
        size + (verify && plan->append[i] ? 4096 : 0);
    uint64_t started;
    uint64_t elapsed;

    error = concurrent_path(path, sizeof(path), worker, i,
                            verify && plan->rename[i]);
    if (error)
      break;
    if (verify && plan->remove[i]) {
      error = backend->ops->stat(backend, path, &status);
      if (error == -ENOENT)
        error = 0;
      else if (!error)
        error = -EEXIST;
      if (error)
        break;
      worker->operations++;
      continue;
    }
    started = bench_now_ns();
    error = backend->ops->open_file(backend, path, 0, &file);
    if (!error)
      error =
          bench_read_all(backend, &file, buffer, (size_t)final_size, NULL);
    if (file.private_data)
      (void)backend->ops->close_file(backend, &file);
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    bench_pattern_fill(expected, (size_t)size, worker->config->seed,
                       0x800000ULL + ((uint64_t)worker->worker << 32) + i, 0);
    if (verify && plan->overwrite[i])
      bench_pattern_fill(expected + (((uint64_t)i * 4096) % size), 4096,
                         worker->config->seed,
                         0xa00000ULL +
                             ((uint64_t)worker->worker << 32) + i,
                         0);
    if (verify && plan->append[i])
      bench_pattern_fill(expected + size, 4096, worker->config->seed,
                         0x900000ULL +
                             ((uint64_t)worker->worker << 32) + i,
                         0);
    if (memcmp(buffer, expected, (size_t)final_size) != 0) {
      error = -EILSEQ;
      break;
    }
    if ((error = bench_latency_add(&worker->latencies, elapsed)) != 0)
      break;
    worker->operations++;
    worker->logical_bytes += final_size;
  }
  return error;
}

static int mixed_worker_edit(struct worker_result *worker,
                             const struct concurrent_mixed_plan *plan,
                             unsigned char *buffer) {
  struct bench_backend *backend = &worker->config->backend;
  uint32_t files = concurrent_mixed_files(worker->config);
  char old_path[BENCH_PATH_MAX];
  char new_path[BENCH_PATH_MAX];
  int error = 0;

  for (uint32_t i = 0; i < files; i++) {
    struct bench_file file = {0};
    uint64_t size = concurrent_mixed_size(worker->config, i);
    uint64_t started;
    uint64_t elapsed;
    int selected = plan->append[i] || plan->overwrite[i] || plan->rename[i] ||
                   plan->remove[i];

    if (!selected)
      continue;
    error = concurrent_path(old_path, sizeof(old_path), worker, i, 0);
    if (error)
      break;
    started = bench_now_ns();
    if (plan->append[i] || plan->overwrite[i]) {
      uint64_t offset =
          plan->append[i] ? size : ((uint64_t)i * 4096) % size;
      uint64_t stream =
          (plan->append[i] ? 0x900000ULL : 0xa00000ULL) +
          ((uint64_t)worker->worker << 32) + i;
      bench_pattern_fill(buffer, 4096, worker->config->seed, stream, 0);
      error = backend->ops->open_file(backend, old_path, 1, &file);
      if (!error)
        error = backend->ops->seek_file(backend, &file, offset);
      if (!error)
        error = bench_write_all(backend, &file, buffer, 4096, NULL);
      if (file.private_data)
        (void)backend->ops->close_file(backend, &file);
      worker->logical_bytes += 4096;
    } else if (plan->rename[i]) {
      error = concurrent_path(new_path, sizeof(new_path), worker, i, 1);
      if (!error)
        error = backend->ops->rename(backend, old_path, new_path);
    } else {
      error = backend->ops->unlink(backend, old_path);
    }
    elapsed = bench_now_ns() - started;
    if (error)
      break;
    if ((error = bench_latency_add(&worker->latencies, elapsed)) != 0)
      break;
    worker->operations++;
  }
  return error;
}

static void *concurrent_mixed_worker(void *opaque) {
  struct worker_result *worker = opaque;
  struct concurrent_mixed_plan plan;
  unsigned char *buffer = NULL;
  unsigned char *expected = NULL;
  uint64_t wall_start;

  memset(&plan, 0, sizeof(plan));
  worker->error = pin_worker_cpu(worker);
  if (worker->error)
    goto ready;
  worker->error = concurrent_plan_create(worker, &plan);
  if (worker->error)
    goto ready;
  buffer = malloc(CONCURRENT_REQUEST + 4096);
  expected = malloc(CONCURRENT_REQUEST + 4096);
  if (!buffer || !expected) {
    worker->error = -ENOMEM;
    goto ready;
  }
  worker->error =
      bench_latency_init(&worker->latencies,
                         concurrent_mixed_files(worker->config));
ready:
  if (start_gate_wait(worker->gate) != 0)
    worker->error = -ECANCELED;
  wall_start = bench_now_ns();
  if (worker->error)
    goto out;
  if (strcmp(worker->phase, "create") == 0)
    worker->error = mixed_worker_create(worker, buffer);
  else if (strcmp(worker->phase, "read") == 0)
    worker->error =
        mixed_worker_read_or_verify(worker, &plan, buffer, expected, 0);
  else if (strcmp(worker->phase, "edit") == 0)
    worker->error = mixed_worker_edit(worker, &plan, buffer);
  else if (strcmp(worker->phase, "verify") == 0)
    worker->error =
        mixed_worker_read_or_verify(worker, &plan, buffer, expected, 1);
  else
    worker->error = -EINVAL;
out:
  worker->elapsed_ns = bench_now_ns() - wall_start;
  free(buffer);
  free(expected);
  concurrent_plan_destroy(&plan);
  return NULL;
}

static int setup_concurrent_mixed(struct bench_run_config *config) {
  uint32_t directories = concurrent_mixed_directories(config);
  char path[BENCH_PATH_MAX];
  int error;

  error =
      config->backend.ops->mkdir(&config->backend, "/concurrent-mixed");
  if (error)
    return error;
  for (unsigned int worker = 0; worker < config->worker_count; worker++) {
    error = bench_format_path(path, sizeof(path), "/concurrent-mixed/w%u",
                              worker);
    if (!error)
      error = config->backend.ops->mkdir(&config->backend, path);
    if (error)
      return error;
    for (uint32_t directory = 0; directory < directories; directory++) {
      error = bench_format_path(path, sizeof(path),
                                "/concurrent-mixed/w%u/d%03u", worker,
                                directory);
      if (!error)
        error = config->backend.ops->mkdir(&config->backend, path);
      if (error)
        return error;
    }
  }
  return 0;
}

static int run_concurrent_mixed_phase(struct bench_run_config *config,
                                      const char *phase) {
  struct start_gate gate;
  pthread_t *threads;
  struct worker_result *workers;
  struct bench_phase_state state;
  char aggregate_phase[80];
  unsigned int created = 0;
  int error = 0;

  if (strcmp(phase, "create") == 0) {
    error = setup_concurrent_mixed(config);
    if (error)
      return error;
  }
  threads = calloc(config->worker_count, sizeof(*threads));
  workers = calloc(config->worker_count, sizeof(*workers));
  if (!threads || !workers) {
    free(threads);
    free(workers);
    return -ENOMEM;
  }
  error = start_gate_init(&gate);
  if (error) {
    free(threads);
    free(workers);
    return error;
  }
  for (unsigned int i = 0; i < config->worker_count; i++) {
    workers[i].config = config;
    workers[i].gate = &gate;
    workers[i].worker = i;
    workers[i].phase = phase;
    if (pthread_create(&threads[i], NULL, concurrent_mixed_worker,
                       &workers[i]) != 0) {
      error = -EAGAIN;
      break;
    }
    created++;
  }
  if (created != config->worker_count) {
    start_gate_release(&gate, 1);
    for (unsigned int i = 0; i < created; i++)
      pthread_join(threads[i], NULL);
    goto out;
  }
  start_gate_await_ready(&gate, created);
  bench_phase_start(config, &state);
  start_gate_release(&gate, 0);
  for (unsigned int i = 0; i < created; i++)
    pthread_join(threads[i], NULL);
  snprintf(aggregate_phase, sizeof(aggregate_phase), "mixed-%s", phase);
  error = emit_concurrency_results(
      config, aggregate_phase, workers, &state,
      strcmp(phase, "create") == 0 || strcmp(phase, "edit") == 0);

out:
  for (unsigned int i = 0; i < created; i++)
    bench_latency_destroy(&workers[i].latencies);
  start_gate_destroy(&gate);
  free(threads);
  free(workers);
  return error;
}

int bench_run_concurrency_seq(struct bench_run_config *config) {
  int error;

  if (bench_phase_selected(config, "write") &&
      (error = run_sequential_phase(config, "write")) != 0)
    return error;
  if (bench_phase_selected(config, "read") &&
      (error = run_sequential_phase(config, "read")) != 0)
    return error;
  return 0;
}

int bench_run_concurrency_mixed(struct bench_run_config *config) {
  static const char *const phases[] = {"create", "read", "edit", "verify"};

  for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
    int error;
    if (bench_phase_selected(config, phases[i]) &&
        (error = run_concurrent_mixed_phase(config, phases[i])) != 0)
      return error;
  }
  return 0;
}
