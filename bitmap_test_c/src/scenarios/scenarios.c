#include "scenarios.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rng.h"

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- dynamic long array ---- */
typedef struct {
    long *data;
    long len;
    long cap;
} larr;

static void larr_init(larr *a) {
    a->data = NULL;
    a->len = 0;
    a->cap = 0;
}

static void larr_free(larr *a) {
    free(a->data);
}

static void larr_extend(larr *a, const long *src, long n) {
    if (a->len + n > a->cap) {
        long nc = a->cap ? a->cap : 16;
        while (a->len + n > nc) {
            nc *= 2;
        }
        a->data = (long *)realloc(a->data, (size_t)nc * sizeof(long));
        a->cap = nc;
    }
    memcpy(&a->data[a->len], src, (size_t)n * sizeof(long));
    a->len += n;
}

static int cmp_long_desc(const void *x, const void *y) {
    long a = *(const long *)x, b = *(const long *)y;
    return (a < b) - (a > b);
}

/* swap-remove the given positions in O(k). descending, so the tail element moved into a hole is
 * never one still pending removal. same bookkeeping as scenarios/harness.py. */
static void larr_remove_positions(larr *a, long *positions, long k) {
    qsort(positions, (size_t)k, sizeof(long), cmp_long_desc);
    for (long i = 0; i < k; i++) {
        long last = a->data[--a->len];
        if (positions[i] < a->len) {
            a->data[positions[i]] = last;
        }
    }
}

static double peak_kib(allocator *al, double cur) {
    double k = al->aux_mem_bytes(al) / 1024.0;
    return k > cur ? k : cur;
}

/* ---- shared mixed driver for stress (0.7) and pareto (0.8) ---- */
static scenario_result run_mixed(const strategy_spec *spec, long bitmap_size,
                                 long blocks_per_alloc, long num_operations,
                                 long seed, double alloc_threshold,
                                 const char *name, const char *mode,
                                 int track_peak) {
    allocator *al = spec->factory(bitmap_size);
    rng_state st;
    rng_seed(&st, (uint64_t)seed);

    double *alloc_times = (double *)malloc((size_t)num_operations * sizeof(double));
    double *dealloc_times = (double *)malloc((size_t)num_operations * sizeof(double));
    long n_alloc = 0, n_dealloc = 0;

    larr allocated;
    larr_init(&allocated);

    long *alloc_buf = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));
    long *free_buf = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));
    long *positions = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));
    double peak_mem = 0.0;

    for (long i = 0; i < num_operations; i++) {
        long estimated_free = bitmap_size - allocated.len;
        double r = rng_double(&st);
        int should_allocate = (r < alloc_threshold) || (allocated.len == 0);

        if (estimated_free <= blocks_per_alloc && allocated.len > 0) {
            should_allocate = 0;
        }

        if (should_allocate) {
            double t0 = now_ns();
            long got = al->allocate(al, blocks_per_alloc, alloc_buf);
            double t1 = now_ns();
            if (got > 0) {
                alloc_times[n_alloc++] = (t1 - t0) / 1000.0;
                larr_extend(&allocated, alloc_buf, got);
            }
        } else {
            long free_count = blocks_per_alloc < allocated.len ? blocks_per_alloc : allocated.len;
            rng_sample_range(&st, allocated.len, free_count, positions);
            for (long j = 0; j < free_count; j++) {
                free_buf[j] = allocated.data[positions[j]];
            }
            double t0 = now_ns();
            bool success = al->deallocate(al, free_buf, free_count);
            double t1 = now_ns();
            if (success) {
                dealloc_times[n_dealloc++] = (t1 - t0) / 1000.0;
                larr_remove_positions(&allocated, positions, free_count);
            }
        }

        if (track_peak) {
            peak_mem = peak_kib(al, peak_mem);
        }
    }

    long free_blocks = al->count_free(al);
    scenario_result res;
    memset(&res, 0, sizeof(res));
    res.m = build_stress_metrics(name, alloc_times, n_alloc, dealloc_times, n_dealloc,
                                 bitmap_size, free_blocks, &al->bm);
    if (mode) {
        res.mode = mode;
        res.has_mode = 1;
    }
    if (track_peak) {
        res.has_peak = 1;
        res.peak_memory_kib = peak_mem;
    }

    free(alloc_times);
    free(dealloc_times);
    free(alloc_buf);
    free(free_buf);
    free(positions);
    larr_free(&allocated);
    al->destroy(al);
    return res;
}

scenario_result scenario_stress(const strategy_spec *spec, long bitmap_size,
                                long blocks_per_alloc, long num_operations,
                                long seed) {
    return run_mixed(spec, bitmap_size, blocks_per_alloc, num_operations, seed,
                     0.7, spec->display_name, NULL, 0);
}

scenario_result scenario_pareto(const strategy_spec *spec, long bitmap_size,
                                long blocks_per_alloc, long num_operations,
                                long seed) {
    char name[256];
    snprintf(name, sizeof(name), "%s (pareto)", spec->display_name);
    return run_mixed(spec, bitmap_size, blocks_per_alloc, num_operations, seed,
                     0.8, name, "pareto", 1);
}

scenario_result scenario_allocation_only(const strategy_spec *spec, long bitmap_size,
                                         long blocks_per_alloc, long num_operations) {
    allocator *al = spec->factory(bitmap_size);
    double *alloc_times = (double *)malloc((size_t)num_operations * sizeof(double));
    long n_alloc = 0;
    long *alloc_buf = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));
    double peak_mem = 0.0;

    for (long i = 0; i < num_operations; i++) {
        double t0 = now_ns();
        long got = al->allocate(al, blocks_per_alloc, alloc_buf);
        double t1 = now_ns();
        if (got > 0) {
            alloc_times[n_alloc++] = (t1 - t0) / 1000.0;
        }
        peak_mem = peak_kib(al, peak_mem);
    }

    long free_blocks = al->count_free(al);
    char name[256];
    snprintf(name, sizeof(name), "%s (allocation_only)", spec->display_name);

    scenario_result res;
    memset(&res, 0, sizeof(res));
    res.m = build_stress_metrics(name, alloc_times, n_alloc, NULL, 0,
                                 bitmap_size, free_blocks, &al->bm);
    res.mode = "allocation_only";
    res.has_mode = 1;
    res.has_peak = 1;
    res.peak_memory_kib = peak_mem;

    free(alloc_times);
    free(alloc_buf);
    al->destroy(al);
    return res;
}

scenario_result scenario_multisize_pareto(const strategy_spec *spec, long bitmap_size,
                                          long num_operations, long seed) {
    static const long SIZES[3] = {256, 2560, 262144};
    allocator *al = spec->factory(bitmap_size);
    rng_state st;
    rng_seed(&st, (uint64_t)seed);

    double *alloc_times = (double *)malloc((size_t)num_operations * sizeof(double));
    double *dealloc_times = (double *)malloc((size_t)num_operations * sizeof(double));
    long n_alloc = 0, n_dealloc = 0;

    larr allocated[3];
    long alloc_counts[3] = {0, 0, 0};
    long dealloc_counts[3] = {0, 0, 0};
    double sum_alloc_sz[3] = {0, 0, 0};
    double sum_dealloc_sz[3] = {0, 0, 0};
    long n_alloc_sz[3] = {0, 0, 0};
    long n_dealloc_sz[3] = {0, 0, 0};
    for (int s = 0; s < 3; s++) {
        larr_init(&allocated[s]);
    }

    long max_size = SIZES[2];
    long *alloc_buf = (long *)malloc((size_t)max_size * sizeof(long));
    long *free_buf = (long *)malloc((size_t)max_size * sizeof(long));
    long *positions = (long *)malloc((size_t)max_size * sizeof(long));
    double peak_mem = 0.0;

    for (long i = 0; i < num_operations; i++) {
        double r = rng_double(&st);
        int si;
        if (r < 0.70) {
            si = 0;
        } else if (r < 0.95) {
            si = 1;
        } else {
            si = 2;
        }
        long file_size = SIZES[si];

        long total_alloc = allocated[0].len + allocated[1].len + allocated[2].len;
        long estimated_free = bitmap_size - total_alloc;
        double r2 = rng_double(&st);
        int should_alloc = (r2 < 0.80) || (allocated[si].len == 0);

        if (estimated_free <= file_size && allocated[si].len > 0) {
            should_alloc = 0;
        }

        if (should_alloc) {
            double t0 = now_ns();
            long got = al->allocate(al, file_size, alloc_buf);
            double t1 = now_ns();
            if (got > 0) {
                double elapsed = (t1 - t0) / 1000.0;
                alloc_times[n_alloc++] = elapsed;
                sum_alloc_sz[si] += elapsed;
                n_alloc_sz[si] += 1;
                larr_extend(&allocated[si], alloc_buf, got);
                alloc_counts[si] += 1;
            }
        } else {
            long free_count = file_size < allocated[si].len ? file_size : allocated[si].len;
            if (free_count > 0) {
                rng_sample_range(&st, allocated[si].len, free_count, positions);
                for (long j = 0; j < free_count; j++) {
                    free_buf[j] = allocated[si].data[positions[j]];
                }
                double t0 = now_ns();
                bool success = al->deallocate(al, free_buf, free_count);
                double t1 = now_ns();
                if (success) {
                    double elapsed = (t1 - t0) / 1000.0;
                    dealloc_times[n_dealloc++] = elapsed;
                    sum_dealloc_sz[si] += elapsed;
                    n_dealloc_sz[si] += 1;
                    larr_remove_positions(&allocated[si], positions, free_count);
                    dealloc_counts[si] += 1;
                }
            }
        }

        peak_mem = peak_kib(al, peak_mem);
    }

    long free_blocks = al->count_free(al);
    char name[256];
    snprintf(name, sizeof(name), "%s (multisize_pareto)", spec->display_name);

    scenario_result res;
    memset(&res, 0, sizeof(res));
    res.m = build_stress_metrics(name, alloc_times, n_alloc, dealloc_times, n_dealloc,
                                 bitmap_size, free_blocks, &al->bm);
    res.mode = "multisize_pareto";
    res.has_mode = 1;
    res.has_peak = 1;
    res.peak_memory_kib = peak_mem;
    res.has_multisize = 1;

    for (int s = 0; s < 3; s++) {
        double total_time_us = sum_alloc_sz[s] + sum_dealloc_sz[s];
        double total_time_s = total_time_us / 1e6;
        long total_ops = alloc_counts[s] + dealloc_counts[s];
        res.alloc_sz[s] = alloc_counts[s];
        res.dealloc_sz[s] = dealloc_counts[s];
        res.ops_per_sec_sz[s] = total_time_s > 0 ? (double)total_ops / total_time_s : 0.0;
        res.total_time_s_sz[s] = total_time_s;
        res.alloc_avg_sz[s] = n_alloc_sz[s] > 0 ? sum_alloc_sz[s] / (double)n_alloc_sz[s] : 0.0;
        res.dealloc_avg_sz[s] = n_dealloc_sz[s] > 0 ? sum_dealloc_sz[s] / (double)n_dealloc_sz[s] : 0.0;
    }

    free(alloc_times);
    free(dealloc_times);
    free(alloc_buf);
    free(free_buf);
    free(positions);
    for (int s = 0; s < 3; s++) {
        larr_free(&allocated[s]);
    }
    al->destroy(al);
    return res;
}
