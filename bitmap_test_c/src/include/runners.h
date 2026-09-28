/*
 * ports of runners/{benchmark,validator,report,statistical}_runner.py.
 */
#ifndef RUNNERS_H
#define RUNNERS_H

#include "registry.h"
#include "scenarios.h"
#include "stats_config.h"

/* BenchmarkRunner.run_demo: fixed 7-step sequence, prints the bitmap after each */
void runner_demo(const strategy_spec *spec, long bitmap_size);

/* BenchmarkRunner.run_stress: StressScenario, seed 42, prints summary. */
scenario_result runner_stress(const strategy_spec *spec, long bitmap_size,
                              long blocks_per_alloc, long num_operations,
                              int show_summary);

/* ValidatorRunner.run: 1 on PASS, 0 on FAIL */
int runner_validate(long bitmap_size, long blocks_per_alloc, long num_operations, long seed);

/* ReportRunner: run all, save report-<ts>.md, resolved path goes into out_path (size cap) */
void runner_report(long bitmap_size, long blocks_per_alloc, long num_operations,
                   char *out_path, long out_path_size);

/* StatisticalRunner.run. strategy_keys NULL means all. CSV path goes into out_runs_path, failure
 * count into *out_failed. 0 on success. */
int runner_statistical(const stat_config *cfg,
                       const char *const *strategy_keys, long n_keys,
                       char *out_runs_path, long out_runs_path_size,
                       long *out_failed);

#endif /* RUNNERS_H */
