#!/usr/bin/env python3
"""
Bitmap Allocators Benchmark Suite
Entry point for running all allocator tests with optional comparison report
"""

import os
import sys

# Add current directory to path for relative imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from allocators import get_strategy_specs
from core import (
    DISK_SIZE_GIB,
    FIXED_BITMAP_SIZE_BLOCKS,
    PAGE_SIZE_KIB,
    StatBenchmarkConfig,
    bitmap_size_blocks_from_disk_gib,
)
from runners import BenchmarkRunner, ReportRunner, ValidatorRunner
from runners import StatisticalRunner

STRATEGY_SPECS = get_strategy_specs()
STRATEGIES = {
    key: (allocator_class, name) for key, allocator_class, name in STRATEGY_SPECS
}


def _menu_select(title, options):
    """Render a simple numbered TUI selector."""
    while True:
        print("\n" + "=" * 72)
        print(title)
        print("=" * 72)
        for i, option in enumerate(options, 1):
            print(f"{i}. {option}")

        choice = input("\nSelect an option: ").strip()
        if choice.isdigit():
            idx = int(choice)
            if 1 <= idx <= len(options):
                return idx - 1
        print("Invalid option. Please try again.")


def run_tui():
    """Interactive terminal UI for selecting benchmark options."""
    strategy_labels = [f"all - All strategies"] + [f"{key} - {name}" for key, (_, name) in STRATEGIES.items()]
    mode_labels = ["Demo + Stress", "Demo only", "Stress only"]

    while True:
        main_idx = _menu_select(
            "Bitmap Allocators Benchmark Suite",
            [
                "Run benchmark",
                "Generate comparison report (.md)",
                "Run consistency validator",
                "Exit",
            ],
        )

        if main_idx == 3:
            print("Bye.")
            return

        if main_idx == 1:
            report_path = run_and_save_report()
            print(f"\nReport saved to: {report_path}")
            continue

        if main_idx == 2:
            passed = run_consistency_validator()
            if not passed:
                sys.exit(1)
            continue

        strategy_idx = _menu_select("Choose strategy", strategy_labels)
        mode_idx = _menu_select("Choose execution mode", mode_labels)

        demo = mode_idx in (0, 1)
        stress = mode_idx in (0, 2)
        strategy = "all" if strategy_idx == 0 else list(STRATEGIES.keys())[strategy_idx - 1]

        if strategy == "all":
            run_all_tests(demo=demo, stress=stress)
        else:
            run_single_test(strategy, demo=demo, stress=stress)


def run_consistency_validator(args=None):
    """Validate that all allocators end with the same bitmap on fixed workload."""
    args = args or []
    bitmap_size = _extract_stat_arg(args, "--validate-bitmap-size", int, None)
    if bitmap_size is not None and bitmap_size <= 0:
        raise ValueError("--validate-bitmap-size must be > 0")
    if bitmap_size is None:
        bitmap_size, _ = _resolve_bitmap_config(args)
    return ValidatorRunner(STRATEGY_SPECS).run(bitmap_size=bitmap_size)


def run_single_test(
    strategy_name,
    demo=False,
    stress=False,
    *,
    bitmap_size=FIXED_BITMAP_SIZE_BLOCKS,
    stress_blocks=2560,
    stress_ops=100,
    workloads=None,
):
    """Run test for a specific strategy."""
    if strategy_name not in STRATEGIES:
        print(f"Unknown strategy: {strategy_name}")
        print(f"Available: {', '.join(STRATEGIES.keys())}")
        return

    allocator_class, name = STRATEGIES[strategy_name]
    runner = BenchmarkRunner(allocator_class, name, bitmap_size=bitmap_size)

    if demo:
        runner.run_demo()
    if stress:
        if workloads:
            for workload in workloads:
                print(
                    f"\n[stress workload] ops={workload['num_operations']}, "
                    f"file_size_mib={workload['file_size_mib']}, "
                    f"blocks={workload['blocks_per_alloc']}"
                )
                runner.run_stress(
                    blocks_per_alloc=workload["blocks_per_alloc"],
                    num_operations=workload["num_operations"],
                )
        else:
            runner.run_stress(
                blocks_per_alloc=stress_blocks,
                num_operations=stress_ops,
            )
    if not demo and not stress:
        runner.run_demo()
        runner.run_stress(
            blocks_per_alloc=stress_blocks,
            num_operations=stress_ops,
        )


def run_all_tests(
    demo=False,
    stress=False,
    *,
    bitmap_size=FIXED_BITMAP_SIZE_BLOCKS,
    stress_blocks=2560,
    stress_ops=100,
    workloads=None,
):
    """Run tests for all strategies."""
    for _, allocator_class, name in STRATEGY_SPECS:
        runner = BenchmarkRunner(allocator_class, name, bitmap_size=bitmap_size)
        if demo:
            runner.run_demo()
        if stress:
            if workloads:
                for workload in workloads:
                    print(
                        f"\n[stress workload] {name}: "
                        f"ops={workload['num_operations']}, "
                        f"file_size_mib={workload['file_size_mib']}, "
                        f"blocks={workload['blocks_per_alloc']}"
                    )
                    runner.run_stress(
                        blocks_per_alloc=workload["blocks_per_alloc"],
                        num_operations=workload["num_operations"],
                    )
            else:
                runner.run_stress(
                    blocks_per_alloc=stress_blocks,
                    num_operations=stress_ops,
                )
        if not demo and not stress:
            runner.run_demo()
            runner.run_stress(
                blocks_per_alloc=stress_blocks,
                num_operations=stress_ops,
            )


def run_and_save_report(
    output_dir=None,
    bitmap_size=FIXED_BITMAP_SIZE_BLOCKS,
    stress_blocks=2560,
    stress_ops=100,
):
    """Run all benchmarks and save markdown report."""
    results = ReportRunner.run_all_benchmarks(
        STRATEGY_SPECS,
        bitmap_size=bitmap_size,
        blocks_per_alloc=stress_blocks,
        num_operations=stress_ops,
    )
    markdown = ReportRunner.build_report_markdown(results)
    return ReportRunner.save_report_file(markdown, output_dir=output_dir)


def _extract_stat_arg(args, name, cast=str, default=None):
    if name not in args:
        return default
    idx = args.index(name)
    if idx + 1 >= len(args):
        raise ValueError(f"Missing value for {name}")
    try:
        return cast(args[idx + 1])
    except ValueError as exc:
        raise ValueError(f"Invalid value for {name}: {args[idx + 1]}") from exc


def _extract_stat_args(args, name):
    values = []
    idx = 0
    while idx < len(args):
        if args[idx] == name:
            if idx + 1 >= len(args):
                raise ValueError(f"Missing value for {name}")
            values.append(args[idx + 1])
            idx += 2
            continue
        idx += 1
    return values


def _resolve_bitmap_config(args):
    bitmap_size = _extract_stat_arg(args, "--bitmap-size", int, None)
    disk_size_gib = _extract_stat_arg(args, "--disk-size-gib", int, DISK_SIZE_GIB)
    if bitmap_size is not None and bitmap_size <= 0:
        raise ValueError("--bitmap-size must be > 0")
    if disk_size_gib <= 0:
        raise ValueError("--disk-size-gib must be > 0")
    if bitmap_size is None:
        bitmap_size = bitmap_size_blocks_from_disk_gib(disk_size_gib, PAGE_SIZE_KIB)
    return bitmap_size, disk_size_gib


def _parse_workloads(args):
    values = _extract_stat_args(args, "--workload")
    workloads = []
    for raw in values:
        if ":" not in raw:
            raise ValueError(f"Invalid --workload format '{raw}'. Expected OPS:FILE_MIB")
        ops_str, file_mib_str = raw.split(":", 1)
        try:
            num_operations = int(ops_str)
            file_size_mib = int(file_mib_str)
        except ValueError as exc:
            raise ValueError(
                f"Invalid --workload value '{raw}'. OPS and FILE_MIB must be integers"
            ) from exc
        if num_operations <= 0:
            raise ValueError("--workload OPS must be > 0")
        if file_size_mib <= 0:
            raise ValueError("--workload FILE_MIB must be > 0")
        blocks_per_alloc = (file_size_mib * 1024) // PAGE_SIZE_KIB
        workloads.append(
            {
                "raw": raw,
                "num_operations": num_operations,
                "file_size_mib": file_size_mib,
                "blocks_per_alloc": blocks_per_alloc,
                "label": f"{num_operations}ops_{file_size_mib}mib",
            }
        )
    return workloads


def run_statistical_benchmark(args):
    mode = _extract_stat_arg(args, "--stat-mode", str, "mixed_7030")
    num_seeds = _extract_stat_arg(args, "--stat-seeds", int, 1000)
    seed_base = _extract_stat_arg(args, "--stat-seed-base", int, 42)
    num_operations = _extract_stat_arg(args, "--stat-ops", int, None)
    output_dir = _extract_stat_arg(args, "--stat-output-dir", str, ".")
    bitmap_size, disk_size_gib = _resolve_bitmap_config(args)
    workloads = _parse_workloads(args)
    strategies_csv = _extract_stat_arg(args, "--stat-strategies", str, None)
    strategy_keys = (
        [item.strip() for item in strategies_csv.split(",") if item.strip()]
        if strategies_csv
        else None
    )
    if workloads and num_operations is not None:
        raise ValueError("--stat-ops cannot be used together with --workload")

    run_results = []
    if workloads:
        for workload in workloads:
            config = StatBenchmarkConfig(
                num_files=100,
                file_size_mib=workload["file_size_mib"],
                block_size_kib=4,
                disk_size_gib=disk_size_gib,
                bitmap_size_override=bitmap_size,
                num_seeds=num_seeds,
                seed_base=seed_base,
                mode=mode,
                num_operations=workload["num_operations"],
                output_dir=output_dir,
                workload_label=workload["label"],
            )
            config.validate()
            run_results.append(
                StatisticalRunner(STRATEGY_SPECS, config).run(
                    strategy_keys=strategy_keys,
                )
            )
    else:
        config = StatBenchmarkConfig(
            num_files=100,
            file_size_mib=10,
            block_size_kib=4,
            disk_size_gib=disk_size_gib,
            bitmap_size_override=bitmap_size,
            num_seeds=num_seeds,
            seed_base=seed_base,
            mode=mode,
            num_operations=num_operations,
            output_dir=output_dir,
            workload_label="default",
        )
        config.validate()
        run_results.append(
            StatisticalRunner(STRATEGY_SPECS, config).run(
                strategy_keys=strategy_keys,
            )
        )

    print("\nArtifacts:")
    for result in run_results:
        print(f"  [{result['workload_label']}] Runs CSV:    {result['runs_path']}")
        print(f"  [{result['workload_label']}] Failed runs: {result['failed_runs']}")


def print_usage():
    """Print usage information"""
    print(f"""
Bitmap Allocators Benchmark Suite
==================================

Usage:
  python run.py                  # Launch interactive TUI
  python run.py [STRATEGY] [OPTIONS]

Strategies:
  avl         - AVL Tree
  rb          - Red-Black Tree
  heap        - Min-Heap
  btree       - B-Tree
  bplustree   - B+Tree
  hybrid      - Hybrid Heap+RB
  segtree     - Segment Tree
  twolevel    - Two-Level Index
  treap       - Treap
  baseline    - Baseline Sequential Scan
  all         - Run all strategies (default)

Options:
  --demo      - Run demo mode only
  --stress    - Run stress test only
  --report    - Run all strategies and generate markdown report file
               (uses --stress-ops and --stress-blocks)
  --bitmap-size N - Bitmap size in blocks (overrides --disk-size-gib)
  --disk-size-gib N - Disk size used to derive bitmap size (default: {DISK_SIZE_GIB})
  --validate  - Run fixed-workload validator across all allocators
  --validate-bitmap-size N - Bitmap size for --validate (overrides global bitmap flags)
  --stress-ops N - Operations for --stress mode (default: 100)
  --stress-blocks N - Blocks per allocation for --stress mode (default: 2560)
  --workload OPS:FILE_MIB - Repeatable workload for --stress/--stat-bench
  --stat-bench - Run statistical benchmark (seeded repeated runs)
  --stat-mode [allocation_only|mixed_7030|multisize_pareto] - Statistical benchmark mode
  --stat-seeds N - Number of random seeds (default: 1000)
  --stat-seed-base N - Base seed to generate reproducible random seed list
  --stat-ops N - Operations per run (default: mode dependent)
  --stat-output-dir DIR - Output directory for CSV
  --stat-strategies a,b,c - Run only selected strategy keys
  --tui       - Launch interactive TUI
  --help      - Show this help message

Examples:
  python run.py                    # Open interactive TUI
  python run.py avl --demo         # Demo only for AVL Tree
  python run.py rb --stress        # Stress test only for Red-Black Tree
  python run.py bplustree --stress # Stress test for B+Tree (fastest!)
  python run.py all --demo         # Demo for all strategies
  python run.py rb --stress --stress-ops 100 --stress-blocks 2560
  python run.py rb --stress --workload 100:10 --workload 10:1024
  python run.py --report           # Generate report-YYYYMMDD-HHMMSS.md
  python run.py --report --stress-ops 100 --stress-blocks 2560
  python run.py --validate         # Verify all allocators end with same bitmap
  python run.py --validate --validate-bitmap-size 1000000
  python run.py --stress --disk-size-gib 250
  python run.py --stress --bitmap-size 1000000
  python run.py --stat-bench --stat-mode allocation_only
  python run.py --stat-bench --stat-mode mixed_7030 --stat-seeds 1000
  python run.py --stat-bench --stat-mode multisize_pareto --stat-seeds 100
  python run.py --stat-bench --workload 100:10 --workload 10:1024

Results:
  Bitmap model (default):
    - Disk size: {DISK_SIZE_GIB} GiB
    - Page size: {PAGE_SIZE_KIB} KiB
    - Mapping: 1 bitmap bit per page
    - Bitmap size: {FIXED_BITMAP_SIZE_BLOCKS:,} pages/bits

  The tests measure:
    - Time to allocate N blocks
    - Time to deallocate N blocks
    - Total operations per second
    - Memory fragmentation patterns
""")


if __name__ == "__main__":
    if "--help" in sys.argv or "-h" in sys.argv:
        print_usage()
        sys.exit(0)

    if "--tui" in sys.argv or (len(sys.argv) == 1 and sys.stdin.isatty()):
        run_tui()
        sys.exit(0)

    if "--report" in sys.argv:
        bitmap_size, _ = _resolve_bitmap_config(sys.argv[1:])
        report_ops = _extract_stat_arg(sys.argv[1:], "--stress-ops", int, 100)
        report_blocks = _extract_stat_arg(sys.argv[1:], "--stress-blocks", int, 2560)
        if report_ops <= 0:
            print("Error: --stress-ops must be > 0")
            sys.exit(1)
        if report_blocks <= 0:
            print("Error: --stress-blocks must be > 0")
            sys.exit(1)
        report_path = run_and_save_report(
            bitmap_size=bitmap_size,
            stress_ops=report_ops,
            stress_blocks=report_blocks,
        )
        print(f"\nReport saved to: {report_path}")
        sys.exit(0)

    if "--validate" in sys.argv:
        try:
            passed = run_consistency_validator(sys.argv[1:])
            sys.exit(0 if passed else 1)
        except ValueError as exc:
            print(f"Error: {exc}")
            sys.exit(1)

    if "--stat-bench" in sys.argv:
        try:
            run_statistical_benchmark(sys.argv[1:])
        except ValueError as exc:
            print(f"Error: {exc}")
            sys.exit(1)
        sys.exit(0)

    demo = "--demo" in sys.argv
    stress = "--stress" in sys.argv
    stress_ops = _extract_stat_arg(sys.argv[1:], "--stress-ops", int, 100)
    stress_blocks = _extract_stat_arg(sys.argv[1:], "--stress-blocks", int, 2560)
    workloads = _parse_workloads(sys.argv[1:])
    bitmap_size, _ = _resolve_bitmap_config(sys.argv[1:])

    if stress_ops <= 0:
        print("Error: --stress-ops must be > 0")
        sys.exit(1)
    if stress_blocks <= 0:
        print("Error: --stress-blocks must be > 0")
        sys.exit(1)
    if workloads and ("--stress" not in sys.argv and "--stat-bench" not in sys.argv):
        print("Error: --workload can only be used with --stress or --stat-bench")
        sys.exit(1)

    if not demo and not stress:
        demo = stress = True

    strategy = None
    for arg in sys.argv[1:]:
        if arg in STRATEGIES:
            strategy = arg
            break

    if strategy:
        run_single_test(
            strategy,
            demo,
            stress,
            bitmap_size=bitmap_size,
            stress_blocks=stress_blocks,
            stress_ops=stress_ops,
            workloads=workloads,
        )
    else:
        run_all_tests(
            demo,
            stress,
            bitmap_size=bitmap_size,
            stress_blocks=stress_blocks,
            stress_ops=stress_ops,
            workloads=workloads,
        )
