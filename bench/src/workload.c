#include "bench_workload.h"
#include "workload_internal.h"

#include <errno.h>
#include <string.h>

int bench_run_workload(struct bench_run_config *config) {
  if (strcmp(config->scenario, "bulk") == 0)
    return bench_run_bulk(config);
  if (strcmp(config->scenario, "metadata") == 0)
    return bench_run_metadata(config);
  if (strcmp(config->scenario, "mixed") == 0)
    return bench_run_mixed(config);
  if (strcmp(config->scenario, "recovery-pressure") == 0)
    return bench_run_recovery(config);
  if (strcmp(config->scenario, "concurrency-seq") == 0)
    return bench_run_concurrency_seq(config);
  if (strcmp(config->scenario, "concurrency-mixed") == 0)
    return bench_run_concurrency_mixed(config);
  return -EINVAL;
}
