/*
 * CLI entry point. Port of run.py (non-interactive subset).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "registry.h"
#include "runners.h"
#include "stats_config.h"

static int has_flag(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* value of token after --flag, or def */
static const char *arg_value(int argc, char **argv, const char *name, const char *def) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            if (i + 1 < argc) {
                return argv[i + 1];
            }
            fprintf(stderr, "Error: Missing value for %s\n", name);
            exit(1);
        }
    }
    return def;
}

static long arg_long(int argc, char **argv, const char *name, long def) {
    const char *v = arg_value(argc, argv, name, NULL);
    if (!v) {
        return def;
    }
    return strtol(v, NULL, 10);
}

static long resolve_bitmap_config(int argc, char **argv) {
    const char *bs = arg_value(argc, argv, "--bitmap-size", NULL);
    long disk = arg_long(argc, argv, "--disk-size-gib", DISK_SIZE_GIB);
    if (bs) {
        long v = strtol(bs, NULL, 10);
        if (v <= 0) {
            fprintf(stderr, "Error: --bitmap-size must be > 0\n");
            exit(1);
        }
        return v;
    }
    if (disk <= 0) {
        fprintf(stderr, "Error: --disk-size-gib must be > 0\n");
        exit(1);
    }
    return bitmap_size_blocks_from_disk_gib(disk, PAGE_SIZE_KIB);
}

static void print_usage(void) {
    printf("Bitmap Allocators Benchmark Suite (C port)\n\n");
    printf("Usage:\n  bitmap_test [STRATEGY] [OPTIONS]\n\n");
    printf("Strategies:\n");
    long n;
    const strategy_spec *specs = registry_specs(&n);
    for (long i = 0; i < n; i++) {
        printf("  %-20s - %s\n", specs[i].key, specs[i].display_name);
    }
    printf("  all                  - Run all strategies\n\n");
    printf("Options:\n");
    printf("  --demo               Run demo mode only\n");
    printf("  --stress             Run stress test only\n");
    printf("  --stress-ops N       Operations for --stress (default 100)\n");
    printf("  --stress-blocks N    Blocks per allocation for --stress (default 2560)\n");
    printf("  --report             Run all strategies and write report-<ts>.md\n");
    printf("  --bitmap-size N      Bitmap size in blocks (overrides --disk-size-gib)\n");
    printf("  --disk-size-gib N    Disk size to derive bitmap size (default %d)\n", DISK_SIZE_GIB);
    printf("  --validate           Run fixed-workload consistency validator\n");
    printf("  --validate-bitmap-size N  Bitmap size for --validate\n");
    printf("  --stat-bench         Run statistical benchmark (seeded repeated runs)\n");
    printf("  --stat-mode MODE     allocation_only | pareto | multisize_pareto\n");
    printf("  --stat-seeds N       Number of seeds (default 1000)\n");
    printf("  --stat-seed-base N   Base seed (default 42)\n");
    printf("  --stat-ops N         Operations per run (mode dependent default)\n");
    printf("  --stat-output-dir D  Output directory for CSV (default .)\n");
    printf("  --stat-strategies a,b,c  Run only selected strategy keys\n");
    printf("  --help               Show this help\n");
}

static long split_keys(char *csv, const char **out, long max) {
    long n = 0;
    char *tok = strtok(csv, ",");
    while (tok && n < max) {
        while (*tok == ' ') {
            tok++;
        }
        if (*tok) {
            out[n++] = tok;
        }
        tok = strtok(NULL, ",");
    }
    return n;
}

int main(int argc, char **argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_usage();
        return 0;
    }

    if (has_flag(argc, argv, "--report")) {
        long bitmap_size = resolve_bitmap_config(argc, argv);
        long ops = arg_long(argc, argv, "--stress-ops", 100);
        long blocks = arg_long(argc, argv, "--stress-blocks", 2560);
        if (ops <= 0 || blocks <= 0) {
            fprintf(stderr, "Error: --stress-ops/--stress-blocks must be > 0\n");
            return 1;
        }
        char path[1024];
        runner_report(bitmap_size, blocks, ops, path, sizeof(path));
        printf("\nReport saved to: %s\n", path);
        return 0;
    }

    if (has_flag(argc, argv, "--validate")) {
        long bitmap_size = arg_long(argc, argv, "--validate-bitmap-size", -1);
        if (bitmap_size == -1) {
            bitmap_size = resolve_bitmap_config(argc, argv);
        } else if (bitmap_size <= 0) {
            fprintf(stderr, "Error: --validate-bitmap-size must be > 0\n");
            return 1;
        }
        int passed = runner_validate(bitmap_size, 5, 10000, 42);
        return passed ? 0 : 1;
    }

    if (has_flag(argc, argv, "--stat-bench")) {
        stat_config cfg = stat_config_default();
        cfg.mode = arg_value(argc, argv, "--stat-mode", "pareto");
        cfg.num_seeds = arg_long(argc, argv, "--stat-seeds", 1000);
        cfg.seed_base = arg_long(argc, argv, "--stat-seed-base", 42);
        long stat_ops = arg_long(argc, argv, "--stat-ops", -1);
        if (stat_ops > 0) {
            cfg.num_operations = stat_ops;
        }
        cfg.output_dir = arg_value(argc, argv, "--stat-output-dir", ".");
        cfg.bitmap_size_override = resolve_bitmap_config(argc, argv);
        cfg.disk_size_gib = arg_long(argc, argv, "--disk-size-gib", DISK_SIZE_GIB);

        const char *keys_buf[64];
        long n_keys = 0;
        const char *sc = arg_value(argc, argv, "--stat-strategies", NULL);
        char keys_copy[512];
        if (sc) {
            strncpy(keys_copy, sc, sizeof(keys_copy) - 1);
            keys_copy[sizeof(keys_copy) - 1] = '\0';
            n_keys = split_keys(keys_copy, keys_buf, 64);
        }

        char runs_path[1024];
        long failed = 0;
        int rc = runner_statistical(&cfg, keys_buf, n_keys, runs_path, sizeof(runs_path), &failed);
        if (rc != 0) {
            return 1;
        }
        printf("\nArtifacts:\n");
        printf("  [%s] Runs CSV:    %s\n", cfg.workload_label, runs_path);
        printf("  [%s] Failed runs: %ld\n", cfg.workload_label, failed);
        return 0;
    }

    /* demo / stress for one or all strategies */
    int demo = has_flag(argc, argv, "--demo");
    int stress = has_flag(argc, argv, "--stress");
    long stress_ops = arg_long(argc, argv, "--stress-ops", 100);
    long stress_blocks = arg_long(argc, argv, "--stress-blocks", 2560);
    long bitmap_size = resolve_bitmap_config(argc, argv);
    if (!demo && !stress) {
        demo = stress = 1;
    }

    /* find strategy token */
    const strategy_spec *chosen = NULL;
    int run_all = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "all") == 0) {
            run_all = 1;
            break;
        }
        const strategy_spec *sp = registry_get(argv[i]);
        if (sp) {
            chosen = sp;
            break;
        }
    }

    long n;
    const strategy_spec *specs = registry_specs(&n);
    if (chosen && !run_all) {
        if (demo) {
            runner_demo(chosen, bitmap_size);
        }
        if (stress) {
            runner_stress(chosen, bitmap_size, stress_blocks, stress_ops, 1);
        }
    } else {
        for (long i = 0; i < n; i++) {
            if (demo) {
                runner_demo(&specs[i], bitmap_size);
            }
            if (stress) {
                runner_stress(&specs[i], bitmap_size, stress_blocks, stress_ops, 1);
            }
        }
    }
    return 0;
}
