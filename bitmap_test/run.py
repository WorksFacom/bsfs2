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
from core import StatBenchmarkConfig
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


def run_consistency_validator():
    """Validate that all allocators end with the same bitmap on fixed workload."""
    return ValidatorRunner(STRATEGY_SPECS).run()


def run_single_test(strategy_name, demo=False, stress=False):
    """Run test for a specific strategy."""
    if strategy_name not in STRATEGIES:
        print(f"Unknown strategy: {strategy_name}")
        print(f"Available: {', '.join(STRATEGIES.keys())}")
        return

    allocator_class, name = STRATEGIES[strategy_name]
    runner = BenchmarkRunner(allocator_class, name)

    if demo:
        runner.run_demo()
    if stress:
        runner.run_stress()
    if not demo and not stress:
        runner.run_demo()
        runner.run_stress()


def run_all_tests(demo=False, stress=False):
    """Run tests for all strategies."""
    for _, allocator_class, name in STRATEGY_SPECS:
        runner = BenchmarkRunner(allocator_class, name)
        if demo:
            runner.run_demo()
        if stress:
            runner.run_stress()
        if not demo and not stress:
            runner.run_demo()
            runner.run_stress()


def run_and_save_report(output_dir=None):
    """Run all benchmarks and save markdown report."""
    results = ReportRunner.run_all_benchmarks(STRATEGY_SPECS)
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


def run_statistical_benchmark(args):
    mode = _extract_stat_arg(args, "--stat-mode", str, "mixed_7030")
    num_seeds = _extract_stat_arg(args, "--stat-seeds", int, 1000)
    seed_base = _extract_stat_arg(args, "--stat-seed-base", int, 42)
    bitmap_multiplier = _extract_stat_arg(args, "--stat-bitmap-multiplier", int, 2)
    num_operations = _extract_stat_arg(args, "--stat-ops", int, None)
    output_dir = _extract_stat_arg(args, "--stat-output-dir", str, ".")
    strategies_csv = _extract_stat_arg(args, "--stat-strategies", str, None)
    strategy_keys = (
        [item.strip() for item in strategies_csv.split(",") if item.strip()]
        if strategies_csv
        else None
    )

    config = StatBenchmarkConfig(
        num_files=100,
        file_size_mib=10,
        block_size_kib=4,
        bitmap_multiplier=bitmap_multiplier,
        num_seeds=num_seeds,
        seed_base=seed_base,
        mode=mode,
        num_operations=num_operations,
        output_dir=output_dir,
    )
    config.validate()

    result = StatisticalRunner(STRATEGY_SPECS, config).run(
        strategy_keys=strategy_keys,
    )

    print("\nArtifacts:")
    print(f"  Runs CSV:    {result['runs_path']}")
    print(f"  Failed runs: {result['failed_runs']}")


def print_usage():
    """Print usage information"""
    print("""
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
  --validate  - Run fixed-workload validator across all allocators
  --stat-bench - Run statistical benchmark (seeded repeated runs)
  --stat-mode [allocation_only|mixed_7030] - Statistical benchmark mode
  --stat-seeds N - Number of random seeds (default: 1000)
  --stat-seed-base N - Base seed to generate reproducible random seed list
  --stat-bitmap-multiplier N - Bitmap multiplier over target blocks (default: 2)
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
  python run.py --report           # Generate report-YYYYMMDD-HHMMSS.md
  python run.py --validate         # Verify all allocators end with same bitmap
  python run.py --stat-bench --stat-mode allocation_only
  python run.py --stat-bench --stat-mode mixed_7030 --stat-seeds 1000

Results:
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
        report_path = run_and_save_report()
        print(f"\nReport saved to: {report_path}")
        sys.exit(0)

    if "--validate" in sys.argv:
        passed = run_consistency_validator()
        sys.exit(0 if passed else 1)

    if "--stat-bench" in sys.argv:
        try:
            run_statistical_benchmark(sys.argv[1:])
        except ValueError as exc:
            print(f"Error: {exc}")
            sys.exit(1)
        sys.exit(0)

    demo = "--demo" in sys.argv
    stress = "--stress" in sys.argv

    if not demo and not stress:
        demo = stress = True

    strategy = None
    for arg in sys.argv[1:]:
        if arg in STRATEGIES:
            strategy = arg
            break

    if strategy:
        run_single_test(strategy, demo, stress)
    else:
        run_all_tests(demo, stress)
