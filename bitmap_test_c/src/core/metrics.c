#include "metrics.h"

#include <string.h>

double calculate_fragmentation(const bitmap *bm) {
    if (bm == NULL || bitmap_count(bm, 1) == 0) {
        return 0.0;
    }

    long total_files = 0;
    long total_frags = 0;
    int in_fragment = 0;

    for (size_t i = 0; i < bm->size; i++) {
        if (bm->data[i] == 1) {
            if (!in_fragment) {
                total_files += 1;
                in_fragment = 1;
            }
            total_frags += 1;
        } else {
            in_fragment = 0;
        }
    }

    if (total_frags == 0) {
        return 0.0;
    }
    return (double)(total_frags - total_files) / (double)total_frags;
}

static double sum_d(const double *v, long n) {
    double s = 0.0;
    for (long i = 0; i < n; i++) {
        s += v[i];
    }
    return s;
}

stress_metrics build_stress_metrics(const char *name,
                                    const double *alloc_us, long n_alloc,
                                    const double *dealloc_us, long n_dealloc,
                                    long bitmap_size, long free_blocks,
                                    const bitmap *bm) {
    stress_metrics m;
    memset(&m, 0, sizeof(m));
    strncpy(m.name, name, sizeof(m.name) - 1);

    double sum_alloc = sum_d(alloc_us, n_alloc);
    double sum_dealloc = sum_d(dealloc_us, n_dealloc);

    m.alloc_avg = n_alloc > 0 ? sum_alloc / (double)n_alloc : 0.0;
    m.dealloc_avg = n_dealloc > 0 ? sum_dealloc / (double)n_dealloc : 0.0;

    double total_time_us = sum_alloc + sum_dealloc;
    m.total_time_s = total_time_us / 1e6;
    long total_ops = n_alloc + n_dealloc;
    m.ops_per_sec = m.total_time_s > 0 ? (double)total_ops / m.total_time_s : 0.0;
    m.avg_time_per_op = total_ops > 0 ? total_time_us / (double)total_ops : 0.0;
    m.num_allocs = n_alloc;
    m.num_deallocs = n_dealloc;
    m.occupied = bitmap_size - free_blocks;
    m.free_blocks = free_blocks;
    m.fragmentation = bm ? calculate_fragmentation(bm) : 0.0;
    return m;
}
