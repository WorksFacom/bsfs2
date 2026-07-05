/*
 * Statistical benchmark configuration. Port of core/stats_config.py.
 */
#ifndef STATS_CONFIG_H
#define STATS_CONFIG_H

#include <stdbool.h>

typedef struct {
    long num_files;          /* 100 */
    long file_size_mib;      /* 10 */
    long block_size_kib;     /* 4 */
    long disk_size_gib;      /* 250 */
    long bitmap_size_override; /* 0 = unset */
    long num_seeds;          /* 1000 */
    long seed_base;          /* 42 */
    const char *mode;        /* "allocation_only" | "pareto" | "multisize_pareto" */
    long num_operations;     /* 0 = unset */
    const char *output_dir;  /* "." */
    const char *workload_label; /* "default" */
} stat_config;

stat_config stat_config_default(void);

long stat_config_blocks_per_file(const stat_config *c);
long stat_config_target_blocks(const stat_config *c);
long stat_config_bitmap_size(const stat_config *c);
long stat_config_resolved_num_operations(const stat_config *c);

/* Returns NULL on success or a static error string describing the violation. */
const char *stat_config_validate(const stat_config *c);

#endif /* STATS_CONFIG_H */
