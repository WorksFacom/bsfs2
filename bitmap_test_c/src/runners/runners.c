#include "runners.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "rng.h"
#include "io_csv.h"
#include "stats.h"

static void fmt_commas(double v, char *buf, long bufsz);

/* ===================== BenchmarkRunner.run_demo ===================== */
static void demo_print_state(allocator *al, const char *desc, const long *alloc, long got, int is_alloc) {
    printf("\n\xE2\x86\x92 %s\n", desc);
    if (is_alloc) {
        printf("  Allocated: [");
        for (long i = 0; i < got; i++) {
            printf("%s%ld", i ? ", " : "", alloc[i]);
        }
        printf("]\n");
    }
    printf("  Bitmap: [");
    long lim = al->size < 50 ? al->size : 50;
    for (long i = 0; i < lim; i++) {
        printf("%d", bitmap_get(&al->bm, (size_t)i));
    }
    printf("...]\n");
    char fb[64];
    fmt_commas((double)al->count_free(al), fb, sizeof(fb));
    printf("  Free blocks: %s\n", fb);
}

void runner_demo(const strategy_spec *spec, long bitmap_size) {
    printf("\n==========================================================================\n");
    printf("DEMO: %s for Bitmap Block Allocation\n", spec->display_name);
    printf("==========================================================================\n");

    allocator *al = spec->factory(bitmap_size);
    long buf[8];
    long got;

    got = al->allocate(al, 3, buf);
    demo_print_state(al, "Allocate 3 blocks", buf, got, 1);

    got = al->allocate(al, 5, buf);
    demo_print_state(al, "Allocate 5 blocks", buf, got, 1);

    long d1[3] = {0, 1, 2};
    al->deallocate(al, d1, 3);
    demo_print_state(al, "Deallocate blocks: [0, 1, 2]", NULL, 0, 0);

    got = al->allocate(al, 2, buf);
    demo_print_state(al, "Allocate 2 blocks", buf, got, 1);

    got = al->allocate(al, 4, buf);
    demo_print_state(al, "Allocate 4 blocks", buf, got, 1);

    long d2[3] = {3, 6, 8};
    al->deallocate(al, d2, 3);
    demo_print_state(al, "Deallocate blocks: [3, 6, 8]", NULL, 0, 0);

    got = al->allocate(al, 2, buf);
    demo_print_state(al, "Allocate 2 blocks", buf, got, 1);

    al->destroy(al);
}

/* ===================== BenchmarkRunner.run_stress ===================== */
scenario_result runner_stress(const strategy_spec *spec, long bitmap_size,
                              long blocks_per_alloc, long num_operations,
                              int show_summary) {
    if (show_summary) {
        printf("\n==========================================================================\n");
        printf("STRESS TEST: %s Multiple Block Allocation Performance\n", spec->display_name);
        printf("==========================================================================\n");
        printf("\nConfiguration:\n");
        printf("  Bitmap size: %ld blocks\n", bitmap_size);
        printf("  Blocks per allocation: %ld\n", blocks_per_alloc);
        printf("  Total operations: %ld\n", num_operations);
        printf("  Operation mix: 70%% allocate / 30%% deallocate\n");
    }
    scenario_result res = scenario_stress(spec, bitmap_size, blocks_per_alloc, num_operations, 42);
    if (show_summary) {
        printf("\nRESULTS\n");
        printf("  Total time: %.3fs\n", res.m.total_time_s);
        printf("  Operations/sec: %.0f\n", res.m.ops_per_sec);
        printf("  Time per operation: %.3f us\n", res.m.avg_time_per_op);
        printf("  Alloc avg: %.3f us (%ld calls)\n", res.m.alloc_avg, res.m.num_allocs);
        printf("  Dealloc avg: %.3f us (%ld calls)\n", res.m.dealloc_avg, res.m.num_deallocs);
        printf("  Occupied: %ld  Free: %ld\n", res.m.occupied, res.m.free_blocks);
        printf("  Fragmentation: %.2f%%\n", res.m.fragmentation * 100.0);
    }
    return res;
}

/* ===================== ValidatorRunner ===================== */
typedef struct {
    long *data;
    long len;
    long cap;
} larr;

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

typedef struct {
    uint8_t *bitmap_copy;   /* reference snapshot (group ref only) */
    long occupied_count;
    long free_count;
    long bitmap_free_count;
    long tracked_occupied_count;
} val_state;

static void run_validation_workload(const strategy_spec *spec, long bitmap_size,
                                    long blocks_per_alloc, long num_operations,
                                    long seed, val_state *out, int keep_copy) {
    allocator *al = spec->factory(bitmap_size);
    rng_state st;
    rng_seed(&st, (uint64_t)seed);
    larr allocated = {NULL, 0, 0};

    long *alloc_buf = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));
    long *positions = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));
    long *to_free = (long *)malloc((size_t)blocks_per_alloc * sizeof(long));

    for (long i = 0; i < num_operations; i++) {
        long estimated_free = bitmap_size - allocated.len;
        double r = rng_double(&st);
        int should_allocate = (r < 0.7) || (allocated.len == 0);
        if (estimated_free <= blocks_per_alloc && allocated.len > 0) {
            should_allocate = 0;
        }

        if (should_allocate) {
            long got = al->allocate(al, blocks_per_alloc, alloc_buf);
            if (got > 0) {
                larr_extend(&allocated, alloc_buf, got);
            }
        } else {
            long free_count = blocks_per_alloc < allocated.len ? blocks_per_alloc : allocated.len;
            rng_sample_range(&st, allocated.len, free_count, positions);
            for (long j = 0; j < free_count; j++) {
                to_free[j] = allocated.data[positions[j]];
            }
            bool success = al->deallocate(al, to_free, free_count);
            if (success) {
                /* remove the sampled positions: mark and compact */
                for (long j = 0; j < free_count; j++) {
                    allocated.data[positions[j]] = -1;
                }
                long w = 0;
                for (long j = 0; j < allocated.len; j++) {
                    if (allocated.data[j] != -1) {
                        allocated.data[w++] = allocated.data[j];
                    }
                }
                allocated.len = w;
            }
        }
    }

    long occupied = (long)bitmap_count(&al->bm, 1);
    out->occupied_count = occupied;
    out->free_count = al->count_free(al);
    out->bitmap_free_count = bitmap_size - occupied;
    out->tracked_occupied_count = allocated.len;
    out->bitmap_copy = NULL;
    if (keep_copy) {
        out->bitmap_copy = (uint8_t *)malloc((size_t)bitmap_size);
        memcpy(out->bitmap_copy, al->bm.data, (size_t)bitmap_size);
    }

    free(alloc_buf);
    free(positions);
    free(to_free);
    free(allocated.data);
    al->destroy(al);
    /* Note: when keep_copy, caller owns out->bitmap_copy. The allocator's own
     * bitmap is freed; comparisons use the snapshot. */
    (void)out;
}

int runner_validate(long bitmap_size, long blocks_per_alloc, long num_operations, long seed) {
    long nspecs;
    const strategy_spec *specs = registry_specs(&nspecs);

    printf("\n================================================================================\n");
    printf("BITMAP ALLOCATORS - CONSISTENCY VALIDATOR\n");
    printf("================================================================================\n");
    printf("\nFixed workload:\n");
    printf("  Bitmap size: %ld blocks\n", bitmap_size);
    printf("  Blocks per allocation: %ld\n", blocks_per_alloc);
    printf("  Operations: %ld\n", num_operations);
    printf("  RNG seed: %ld\n\n", seed);

    /* group references: group name -> (ref name, snapshot) */
    #define MAX_GROUPS 8
    const char *group_names[MAX_GROUPS];
    const char *group_ref_name[MAX_GROUPS];
    uint8_t *group_ref_bm[MAX_GROUPS];
    long ngroups = 0;

    int consistency_failed = 0;
    int divergence_failed = 0;

    for (long s = 0; s < nspecs; s++) {
        /* Build allocator briefly to read its consistency group. */
        allocator *probe = specs[s].factory(1);
        const char *group = probe->consistency_group;
        probe->destroy(probe);

        /* Find group slot. */
        long gi = -1;
        for (long g = 0; g < ngroups; g++) {
            if (strcmp(group_names[g], group) == 0) {
                gi = g;
                break;
            }
        }

        val_state vs;
        run_validation_workload(&specs[s], bitmap_size, blocks_per_alloc, num_operations, seed, &vs, 1);

        if (vs.free_count != vs.bitmap_free_count) {
            printf("- %s: count_free_blocks=%ld != bitmap_free=%ld\n",
                   specs[s].display_name, vs.free_count, vs.bitmap_free_count);
            consistency_failed = 1;
        }
        if (vs.tracked_occupied_count != vs.occupied_count) {
            printf("- %s: tracked_occupied=%ld != bitmap_occupied=%ld\n",
                   specs[s].display_name, vs.tracked_occupied_count, vs.occupied_count);
            consistency_failed = 1;
        }

        if (gi == -1) {
            /* first member of group: keep snapshot as reference */
            gi = ngroups++;
            group_names[gi] = group;
            group_ref_name[gi] = specs[s].display_name;
            group_ref_bm[gi] = vs.bitmap_copy;
        } else {
            /* divergence: compare snapshot to group reference */
            long mc = 0, first = -1;
            for (long i = 0; i < bitmap_size; i++) {
                if (vs.bitmap_copy[i] != group_ref_bm[gi][i]) {
                    if (first == -1) {
                        first = i;
                    }
                    mc++;
                }
            }
            if (mc != 0) {
                printf("- %s divergiu de %s [grupo %s]: %ld posicoes diferentes (primeira em idx=%ld)\n",
                       specs[s].display_name, group_ref_name[gi], group, mc, first);
                divergence_failed = 1;
            }
            free(vs.bitmap_copy);
        }
        printf("  \xE2\x9C\x93 %s\n", specs[s].display_name);
    }

    for (long g = 0; g < ngroups; g++) {
        free(group_ref_bm[g]);
    }

    printf("\n--------------------------------------------------------------------------------\n");
    if (consistency_failed) {
        printf("INTERNAL CONSISTENCY CHECKS: FAILED\n");
    } else {
        printf("INTERNAL CONSISTENCY CHECKS: OK\n");
    }
    if (divergence_failed) {
        printf("BITMAP COMPARISON: FAILED\nValidator result: FAIL\n");
        return 0;
    }
    printf("BITMAP COMPARISON: OK\nValidator result: PASS\n");
    return consistency_failed ? 0 : 1;
}

/* ===================== ReportRunner ===================== */
static void fmt_commas(double v, char *buf, long bufsz) {
    long long n = (long long)(v < 0 ? v - 0.5 : v + 0.5);
    char tmp[64];
    int neg = n < 0;
    unsigned long long u = neg ? (unsigned long long)(-n) : (unsigned long long)n;
    int len = snprintf(tmp, sizeof(tmp), "%llu", u);
    /* insert commas */
    int commas = (len - 1) / 3;
    int out_len = len + commas + (neg ? 1 : 0);
    if (out_len >= bufsz) {
        snprintf(buf, (size_t)bufsz, "%lld", n);
        return;
    }
    int bi = 0;
    if (neg) {
        buf[bi++] = '-';
    }
    int first = len % 3;
    if (first == 0) {
        first = 3;
    }
    int i = 0;
    for (; i < len; i++) {
        if (i != 0 && (i - first) % 3 == 0) {
            buf[bi++] = ',';
        }
        buf[bi++] = tmp[i];
    }
    buf[bi] = '\0';
}

static int cmp_ops_desc(const void *a, const void *b) {
    const stress_metrics *x = (const stress_metrics *)a;
    const stress_metrics *y = (const stress_metrics *)b;
    if (x->ops_per_sec < y->ops_per_sec) {
        return 1;
    }
    if (x->ops_per_sec > y->ops_per_sec) {
        return -1;
    }
    return 0;
}

void runner_report(long bitmap_size, long blocks_per_alloc, long num_operations,
                   char *out_path, long out_path_size) {
    long nspecs;
    const strategy_spec *specs = registry_specs(&nspecs);

    printf("\n================================================================================\n");
    printf("BITMAP ALLOCATORS - COMPREHENSIVE BENCHMARK SUITE\n");
    printf("================================================================================\n");
    printf("\nRunning benchmarks for all %ld strategies...\n\n", nspecs);

    stress_metrics *results = (stress_metrics *)malloc((size_t)nspecs * sizeof(stress_metrics));
    for (long i = 0; i < nspecs; i++) {
        printf("[%ld/%ld] Testing %s... ", i + 1, nspecs, specs[i].display_name);
        fflush(stdout);
        scenario_result r = scenario_stress(&specs[i], bitmap_size, blocks_per_alloc, num_operations, 42);
        results[i] = r.m;
        printf("\xE2\x9C\x93\n");
    }

    stress_metrics *sorted = (stress_metrics *)malloc((size_t)nspecs * sizeof(stress_metrics));
    memcpy(sorted, results, (size_t)nspecs * sizeof(stress_metrics));
    qsort(sorted, (size_t)nspecs, sizeof(stress_metrics), cmp_ops_desc);

    double winner_ops = sorted[0].ops_per_sec;
    double second_ops = nspecs > 1 ? sorted[1].ops_per_sec : sorted[0].ops_per_sec;
    double gain = second_ops > 0 ? (winner_ops / second_ops - 1.0) * 100.0 : 0.0;

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char gen_at[64];
    strftime(gen_at, sizeof(gen_at), "%Y-%m-%d %H:%M:%S", &tmv);

    /* Build markdown */
    char path[512];
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);
    snprintf(path, sizeof(path), "report-%s.md", ts);
    FILE *f = fopen(path, "wb");

    fprintf(f, "# Bitmap Allocators Benchmark Report\n\n");
    fprintf(f, "- Generated at: `%s`\n", gen_at);
    fprintf(f, "- Strategies: `%ld`\n\n", nspecs);
    fprintf(f, "## Throughput Ranking\n\n| Rank | Strategy | Ops/sec |\n|---|---|---:|\n");
    char cbuf[64];
    for (long i = 0; i < nspecs; i++) {
        fmt_commas(sorted[i].ops_per_sec, cbuf, sizeof(cbuf));
        fprintf(f, "| %ld | %s | %s |\n", i + 1, sorted[i].name, cbuf);
    }
    fprintf(f, "\n## Detailed Metrics\n\n");
    fprintf(f, "| Strategy | Ops/sec | Alloc avg (\xC2\xB5s) | Dealloc avg (\xC2\xB5s) | Fragmentation | Total time (s) |\n");
    fprintf(f, "|---|---:|---:|---:|---:|---:|\n");
    for (long i = 0; i < nspecs; i++) {
        fmt_commas(results[i].ops_per_sec, cbuf, sizeof(cbuf));
        fprintf(f, "| %s | %s | %.2f | %.2f | %.4f | %.3f |\n",
                results[i].name, cbuf, results[i].alloc_avg, results[i].dealloc_avg,
                results[i].fragmentation, results[i].total_time_s);
    }
    fmt_commas(winner_ops, cbuf, sizeof(cbuf));
    fprintf(f, "\n## Winner\n\n- **%s**\n- Throughput: **%s ops/sec**\n- Advantage over #2: **%.1f%%**\n\n",
            sorted[0].name, cbuf, gain);
    fclose(f);

    /* resolve absolute path */
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(out_path, (size_t)out_path_size, "%s/%s", cwd, path);
    } else {
        snprintf(out_path, (size_t)out_path_size, "%s", path);
    }

    free(results);
    free(sorted);
}

/* ===================== StatisticalRunner ===================== */
static char *dupf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *s = (char *)malloc(strlen(buf) + 1);
    strcpy(s, buf);
    return s;
}

static const char *DEFAULT_HEADERS[] = {
    "workload[label]", "workload_file_size_mib[MiB]", "workload_ops[count]", "seed[id]",
    "mode[label]", "strategy_key[label]", "strategy_name[label]", "status[label]", "error[text]",
    "bitmap_size[blocks]", "target_blocks[blocks]", "blocks_per_file[blocks/file]", "num_operations[ops]",
    "ops_per_sec[ops/s]", "total_time[s]", "alloc_avg[us]", "dealloc_avg[us]", "num_allocs[calls]",
    "num_deallocs[calls]", "avg_time_per_op[us/op]", "occupied[blocks]", "free_blocks[blocks]",
    "fragmentation[ratio]", "peak_memory[KiB]",
};
#define DEFAULT_NCOLS 24

static const char *MULTISIZE_HEADERS[] = {
    "seed[id]", "mode[label]", "strategy_key[label]", "strategy_name[label]", "bitmap_size[blocks]",
    "num_operations[ops]", "ops_per_sec[ops/s]", "status[label]", "error[text]", "total_time[s]",
    "alloc_avg[us]", "dealloc_avg[us]", "num_allocs[calls]", "num_deallocs[calls]",
    "alloc_1mib[calls]", "dealloc_1mib[calls]", "ops_per_sec_1mib[ops/s]", "total_time_1mib[s]",
    "alloc_avg_1mib[us]", "dealloc_avg_1mib[us]",
    "alloc_10mib[calls]", "dealloc_10mib[calls]", "ops_per_sec_10mib[ops/s]", "total_time_10mib[s]",
    "alloc_avg_10mib[us]", "dealloc_avg_10mib[us]",
    "alloc_1gib[calls]", "dealloc_1gib[calls]", "ops_per_sec_1gib[ops/s]", "total_time_1gib[s]",
    "alloc_avg_1gib[us]", "dealloc_avg_1gib[us]",
    "occupied[blocks]", "free_blocks[blocks]", "fragmentation[ratio]", "peak_memory[KiB]",
};
#define MULTISIZE_NCOLS 36

int runner_statistical(const stat_config *cfg,
                       const char *const *strategy_keys, long n_keys,
                       char *out_runs_path, long out_runs_path_size,
                       long *out_failed) {
    const char *err = stat_config_validate(cfg);
    if (err) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    long nspecs;
    const strategy_spec *allspecs = registry_specs(&nspecs);

    /* filter specs */
    const strategy_spec **specs = (const strategy_spec **)malloc((size_t)nspecs * sizeof(*specs));
    long nsel = 0;
    for (long i = 0; i < nspecs; i++) {
        int keep = (n_keys == 0);
        for (long k = 0; k < n_keys; k++) {
            if (strcmp(allspecs[i].key, strategy_keys[k]) == 0) {
                keep = 1;
                break;
            }
        }
        if (keep) {
            specs[nsel++] = &allspecs[i];
        }
    }
    if (nsel == 0) {
        fprintf(stderr, "Error: No strategies selected for statistical benchmark\n");
        free(specs);
        return 1;
    }

    long num_ops = stat_config_resolved_num_operations(cfg);
    long bitmap_size = stat_config_bitmap_size(cfg);
    long blocks_per_file = stat_config_blocks_per_file(cfg);
    long target_blocks = stat_config_target_blocks(cfg);
    int is_multisize = strcmp(cfg->mode, "multisize_pareto") == 0;

    long *seeds = generate_seeds(cfg->num_seeds, cfg->seed_base);

    printf("\n================================================================================\n");
    printf("BITMAP ALLOCATORS - STATISTICAL BENCHMARK\n");
    printf("================================================================================\n");
    printf("Workload: %s\nMode: %s\nStrategies: %ld\nSeeds: %ld\n",
           cfg->workload_label, cfg->mode, nsel, cfg->num_seeds);
    printf("Bitmap size: %ld blocks\nBlocks per allocation (file): %ld\nOperations per run: %ld\n\n",
           bitmap_size, blocks_per_file, num_ops);

    long ncols = is_multisize ? MULTISIZE_NCOLS : DEFAULT_NCOLS;
    long total_runs = cfg->num_seeds * nsel;
    char **cells = (char **)malloc((size_t)total_runs * (size_t)ncols * sizeof(char *));
    long row = 0;
    long done = 0;

    for (long si = 0; si < cfg->num_seeds; si++) {
        long seed = seeds[si];
        for (long pi = 0; pi < nsel; pi++) {
            const strategy_spec *spec = specs[pi];
            scenario_result r;
            if (strcmp(cfg->mode, "allocation_only") == 0) {
                r = scenario_allocation_only(spec, bitmap_size, blocks_per_file, num_ops);
            } else if (is_multisize) {
                r = scenario_multisize_pareto(spec, bitmap_size, num_ops, seed);
            } else {
                r = scenario_pareto(spec, bitmap_size, blocks_per_file, num_ops, seed);
            }

            char **c = &cells[row * ncols];
            if (is_multisize) {
                c[0] = dupf("%ld", seed);
                c[1] = dupf("%s", cfg->mode);
                c[2] = dupf("%s", spec->key);
                c[3] = dupf("%s", spec->display_name);
                c[4] = dupf("%ld", bitmap_size);
                c[5] = dupf("%ld", num_ops);
                c[6] = dupf("%.10g", r.m.ops_per_sec);
                c[7] = dupf("ok");
                c[8] = dupf("");
                c[9] = dupf("%.10g", r.m.total_time_s);
                c[10] = dupf("%.10g", r.m.alloc_avg);
                c[11] = dupf("%.10g", r.m.dealloc_avg);
                c[12] = dupf("%ld", r.m.num_allocs);
                c[13] = dupf("%ld", r.m.num_deallocs);
                int base[3] = {14, 20, 26};
                for (int s = 0; s < 3; s++) {
                    int b = base[s];
                    c[b + 0] = dupf("%ld", r.alloc_sz[s]);
                    c[b + 1] = dupf("%ld", r.dealloc_sz[s]);
                    c[b + 2] = dupf("%.10g", r.ops_per_sec_sz[s]);
                    c[b + 3] = dupf("%.10g", r.total_time_s_sz[s]);
                    c[b + 4] = dupf("%.10g", r.alloc_avg_sz[s]);
                    c[b + 5] = dupf("%.10g", r.dealloc_avg_sz[s]);
                }
                c[32] = dupf("%ld", r.m.occupied);
                c[33] = dupf("%ld", r.m.free_blocks);
                c[34] = dupf("%.10g", r.m.fragmentation);
                c[35] = dupf("%.10g", r.peak_memory_kib);
            } else {
                c[0] = dupf("%s", cfg->workload_label);
                c[1] = dupf("%ld", cfg->file_size_mib);
                c[2] = dupf("%ld", num_ops);
                c[3] = dupf("%ld", seed);
                c[4] = dupf("%s", cfg->mode);
                c[5] = dupf("%s", spec->key);
                c[6] = dupf("%s", spec->display_name);
                c[7] = dupf("ok");
                c[8] = dupf("");
                c[9] = dupf("%ld", bitmap_size);
                c[10] = dupf("%ld", target_blocks);
                c[11] = dupf("%ld", blocks_per_file);
                c[12] = dupf("%ld", num_ops);
                c[13] = dupf("%.10g", r.m.ops_per_sec);
                c[14] = dupf("%.10g", r.m.total_time_s);
                c[15] = dupf("%.10g", r.m.alloc_avg);
                c[16] = dupf("%.10g", r.m.dealloc_avg);
                c[17] = dupf("%ld", r.m.num_allocs);
                c[18] = dupf("%ld", r.m.num_deallocs);
                c[19] = dupf("%.10g", r.m.avg_time_per_op);
                c[20] = dupf("%ld", r.m.occupied);
                c[21] = dupf("%ld", r.m.free_blocks);
                c[22] = dupf("%.10g", r.m.fragmentation);
                c[23] = dupf("%.10g", r.has_peak ? r.peak_memory_kib : 0.0);
            }
            row++;
            done++;
            long step = total_runs / 20;
            if (step < 1) {
                step = 1;
            }
            if (done % step == 0) {
                printf("Progress: %ld/%ld\n", done, total_runs);
            }
        }
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char ts[64];
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y%m%d-%H%M%S", &tmv);
    snprintf(ts, sizeof(ts), "%s-%06ld", tsbuf, (long)tv.tv_usec);

    char path[1024];
    if (strcmp(cfg->output_dir, ".") == 0) {
        snprintf(path, sizeof(path), "benchmark_runs_%s_%s.csv", cfg->mode, ts);
    } else {
        snprintf(path, sizeof(path), "%s/benchmark_runs_%s_%s.csv", cfg->output_dir, cfg->mode, ts);
    }

    const char **headers = is_multisize ? MULTISIZE_HEADERS : DEFAULT_HEADERS;
    csv_write(path, headers, ncols, (const char *const *)cells, total_runs);

    for (long i = 0; i < total_runs * ncols; i++) {
        free(cells[i]);
    }
    free(cells);
    free(seeds);
    free(specs);

    if (out_failed) {
        *out_failed = 0;
    }
    char cwd[512];
    if (path[0] != '/' && getcwd(cwd, sizeof(cwd))) {
        snprintf(out_runs_path, (size_t)out_runs_path_size, "%s/%s", cwd, path);
    } else {
        snprintf(out_runs_path, (size_t)out_runs_path_size, "%s", path);
    }
    return 0;
}
