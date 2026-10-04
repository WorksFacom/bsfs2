#define _GNU_SOURCE
#include "bench_backend.h"
#include "bench_workload.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *output, const char *program) {
  fprintf(output,
          "Usage: %s --root PATH --scenario NAME [options]\n"
          "  --phase NAME                phase or all (default: all)\n"
          "  --seed N                    deterministic seed\n"
          "  --repetition N              measured repetition number\n"
          "  --workers N                 worker count\n"
          "  --results PATH              JSON Lines output\n"
          "  --artifact-dir PATH         raw sidecar directory\n"
          "  --block-device PATH         device statistics source\n"
          "  --reclaim-stats PATH        reclaim counter source\n"
          "  --filesystem NAME           result label\n"
          "  --filesystem-version TEXT\n"
          "  --filesystem-options TEXT\n"
          "  --run-id ID\n"
          "  --cpu N                     pin runner to one CPU\n"
          "  --cpu-list N[,N...]         one configured CPU per worker\n",
          program);
}

static int parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  unsigned long long value;

  /* strtoull silently wraps a leading '-', so reject the sign outright */
  if (!text || *text == '-' || *text == '+' || *text == '\0')
    return -EINVAL;
  errno = 0;
  value = strtoull(text, &end, 10);
  if (errno || !end || *end)
    return -EINVAL;
  *out = value;
  return 0;
}

static int pin_cpu(const char *text) {
  uint64_t cpu;
  cpu_set_t set;
  int error = parse_u64(text, &cpu);

  if (error)
    return error;
  if (cpu >= CPU_SETSIZE)
    return -ERANGE;
  CPU_ZERO(&set);
  CPU_SET((unsigned int)cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    return -errno;
  return 0;
}

static int parse_cpu_list(const char *text, struct bench_run_config *config) {
  char *copy = strdup(text);
  char *cursor;
  char *save = NULL;

  if (!copy)
    return -ENOMEM;
  for (cursor = strtok_r(copy, ",", &save); cursor;
       cursor = strtok_r(NULL, ",", &save)) {
    uint64_t cpu;
    int error;

    if (config->cpu_count >= 256) {
      free(copy);
      return -E2BIG;
    }
    error = parse_u64(cursor, &cpu);
    if (error || cpu >= CPU_SETSIZE) {
      free(copy);
      return error ? error : -ERANGE;
    }
    config->cpus[config->cpu_count++] = (unsigned int)cpu;
  }
  free(copy);
  return config->cpu_count ? 0 : -EINVAL;
}

int main(int argc, char **argv) {
  struct bench_run_config config = {
      .phase = "all",
      .result_path = "results.jsonl",
      .filesystem = "unknown",
      .filesystem_version = "unknown",
      .filesystem_options = "",
      .run_id = "manual",
      .seed = 1234,
      .worker_count = 1,
  };
  const char *cpu = NULL;
  int option;
  int error;
  static const struct option options[] = {
      {"root", required_argument, NULL, 'r'},
      {"scenario", required_argument, NULL, 's'},
      {"phase", required_argument, NULL, 'p'},
      {"seed", required_argument, NULL, 'S'},
      {"repetition", required_argument, NULL, 'R'},
      {"workers", required_argument, NULL, 'w'},
      {"results", required_argument, NULL, 'o'},
      {"artifact-dir", required_argument, NULL, 'a'},
      {"block-device", required_argument, NULL, 'b'},
      {"reclaim-stats", required_argument, NULL, 'c'},
      {"filesystem", required_argument, NULL, 'f'},
      {"filesystem-version", required_argument, NULL, 'v'},
      {"filesystem-options", required_argument, NULL, 'O'},
      {"run-id", required_argument, NULL, 'i'},
      {"cpu", required_argument, NULL, 'C'},
      {"cpu-list", required_argument, NULL, 'L'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
    uint64_t value;
    switch (option) {
    case 'r':
      config.target = optarg;
      break;
    case 's':
      config.scenario = optarg;
      break;
    case 'p':
      config.phase = optarg;
      break;
    case 'S':
      if (parse_u64(optarg, &config.seed))
        return 2;
      break;
    case 'R':
      if (parse_u64(optarg, &value) || value > UINT32_MAX)
        return 2;
      config.repetition = (unsigned int)value;
      break;
    case 'w':
      if (parse_u64(optarg, &value) || value == 0 || value > 256)
        return 2;
      config.worker_count = (unsigned int)value;
      break;
    case 'o':
      config.result_path = optarg;
      break;
    case 'a':
      config.artifact_dir = optarg;
      break;
    case 'b':
      config.block_device = optarg;
      break;
    case 'c':
      config.reclaim_stats_path = optarg;
      break;
    case 'f':
      config.filesystem = optarg;
      break;
    case 'v':
      config.filesystem_version = optarg;
      break;
    case 'O':
      config.filesystem_options = optarg;
      break;
    case 'i':
      config.run_id = optarg;
      break;
    case 'C':
      cpu = optarg;
      break;
    case 'L':
      if (parse_cpu_list(optarg, &config) != 0)
        return 2;
      break;
    case 'h':
      usage(stdout, argv[0]);
      return 0;
    default:
      usage(stderr, argv[0]);
      return 2;
    }
  }

  if (!config.target || !config.scenario) {
    usage(stderr, argv[0]);
    return 2;
  }
  if (config.artifact_dir && mkdir(config.artifact_dir, 0755) != 0 &&
      errno != EEXIST) {
    perror("artifact directory");
    return 2;
  }
  if (cpu && (error = pin_cpu(cpu)) != 0) {
    fprintf(stderr, "cannot pin CPU: %s\n", strerror(-error));
    return 2;
  }
  error = bench_backend_create(&config.backend);
  if (error) {
    fprintf(stderr, "cannot create backend: %s\n", strerror(-error));
    return 2;
  }
  error = config.backend.ops->open(&config.backend, config.target);
  if (error) {
    fprintf(stderr, "cannot open benchmark target: %s\n", strerror(-error));
    bench_backend_destroy(&config.backend);
    return 2;
  }
  error = bench_run_workload(&config);
  bench_backend_destroy(&config.backend);
  if (error) {
    fprintf(stderr, "benchmark failed: %s\n",
            strerror(error < 0 ? -error : error));
    return 1;
  }
  return 0;
}
