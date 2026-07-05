/*
 * Metrics aggregation. Port of core/metrics.py.
 */
#ifndef METRICS_H
#define METRICS_H

#include "bitmap.h"

typedef struct {
    char name[256];
    double ops_per_sec;
    double total_time_s;
    double alloc_avg;   /* us */
    double dealloc_avg; /* us */
    long num_allocs;
    long num_deallocs;
    double avg_time_per_op; /* us/op */
    long occupied;
    long free_blocks;
    double fragmentation;
} stress_metrics;

/* (total_frags - total_files) / total_frags over contiguous runs of set bits;
 * 0.0 when bm is NULL or empty. */
double calculate_fragmentation(const bitmap *bm);

/* Pass bm = NULL to force fragmentation = 0.0 (Python bitmap=None default). */
stress_metrics build_stress_metrics(const char *name,
                                     const double *alloc_us, long n_alloc,
                                     const double *dealloc_us, long n_dealloc,
                                     long bitmap_size, long free_blocks,
                                     const bitmap *bm);

#endif /* METRICS_H */
