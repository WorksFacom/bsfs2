/*
 * ports of scenarios/{stress,pareto,allocation_only,multisize_pareto}.py. each builds an allocator
 * from a spec, runs the workload and returns aggregated metrics.
 */
#ifndef SCENARIOS_H
#define SCENARIOS_H

#include "metrics.h"
#include "registry.h"

typedef struct {
    stress_metrics m;
    const char *mode;       /* NULL for plain stress (no "mode" field) */
    int has_mode;
    int has_peak;
    double peak_memory_kib;

    int has_multisize;
    /* index 0 = 1mib (256), 1 = 10mib (2560), 2 = 1gib (262144) */
    long alloc_sz[3];
    long dealloc_sz[3];
    double ops_per_sec_sz[3];
    double total_time_s_sz[3];
    double alloc_avg_sz[3];
    double dealloc_avg_sz[3];
} scenario_result;

/* StressScenario: 70/30 mix. name == display_name (no suffix). */
scenario_result scenario_stress(const strategy_spec *spec, long bitmap_size,
                                 long blocks_per_alloc, long num_operations,
                                 long seed);

/* ParetoScenario: 80/20 mix, name "<display> (pareto)". */
scenario_result scenario_pareto(const strategy_spec *spec, long bitmap_size,
                                long blocks_per_alloc, long num_operations,
                                long seed);

/* AllocationOnlyScenario: sequential allocations, no RNG. */
scenario_result scenario_allocation_only(const strategy_spec *spec, long bitmap_size,
                                         long blocks_per_alloc, long num_operations);

/* MultisizeParetoScenario: 1/10/1024 MiB, 80/20. */
scenario_result scenario_multisize_pareto(const strategy_spec *spec, long bitmap_size,
                                          long num_operations, long seed);

#endif /* SCENARIOS_H */
