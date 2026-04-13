#!/usr/bin/env python3
"""
Bitmap Allocators Benchmark Suite with markdown report output.
"""

from datetime import datetime
from pathlib import Path
import sys

from run_all import STRATEGY_SPECS, BitmapTester


def run_all_benchmarks():
    """Run all strategies and collect results."""
    results = []
    total = len(STRATEGY_SPECS)

    print("\n" + "=" * 80)
    print("BITMAP ALLOCATORS - COMPREHENSIVE BENCHMARK SUITE")
    print("=" * 80)
    print(f"\nRunning benchmarks for all {total} strategies...\n")

    for i, (_, allocator_class, name) in enumerate(STRATEGY_SPECS, 1):
        print(f"[{i}/{total}] Testing {name}...", end=" ", flush=True)
        tester = BitmapTester(allocator_class, name)
        results.append(tester.stress_test(show_progress=False, show_summary=False))
        print("✓")

    return results


def build_report_markdown(results):
    """Build report markdown content."""
    generated_at = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    sorted_results = sorted(results, key=lambda x: x["ops_per_sec"], reverse=True)
    winner = sorted_results[0]
    second = sorted_results[1] if len(sorted_results) > 1 else sorted_results[0]
    gain = (winner["ops_per_sec"] / second["ops_per_sec"] - 1) * 100 if second["ops_per_sec"] > 0 else 0.0

    lines = [
        "# Bitmap Allocators Benchmark Report",
        "",
        f"- Generated at: `{generated_at}`",
        f"- Strategies: `{len(results)}`",
        "",
        "## Throughput Ranking",
        "",
        "| Rank | Strategy | Ops/sec |",
        "|---|---|---:|",
    ]

    for rank, result in enumerate(sorted_results, 1):
        lines.append(f"| {rank} | {result['name']} | {result['ops_per_sec']:,.0f} |")

    lines.extend(
        [
            "",
            "## Detailed Metrics",
            "",
            "| Strategy | Ops/sec | Alloc avg (µs) | Alloc p95 (µs) | Dealloc avg (µs) | Dealloc p95 (µs) | Total time (s) |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )

    for result in results:
        lines.append(
            f"| {result['name']} | {result['ops_per_sec']:,.0f} | {result['alloc_avg']:.2f} | {result['alloc_p95']:.2f} | "
            f"{result['dealloc_avg']:.2f} | {result['dealloc_p95']:.2f} | {result['total_time_s']:.3f} |"
        )

    lines.extend(
        [
            "",
            "## Winner",
            "",
            f"- **{winner['name']}**",
            f"- Throughput: **{winner['ops_per_sec']:,.0f} ops/sec**",
            f"- Advantage over #2: **{gain:.1f}%**",
            "",
        ]
    )

    return "\n".join(lines)


def save_report_file(markdown, output_dir=None):
    """Save report markdown file and return absolute path."""
    directory = Path(output_dir) if output_dir else Path.cwd()
    directory.mkdir(parents=True, exist_ok=True)
    filename = f"report-{datetime.now().strftime('%Y%m%d-%H%M%S')}.md"
    output_path = directory / filename
    output_path.write_text(markdown, encoding="utf-8")
    return output_path.resolve()


def generate_comparison_report(results, output_dir=None):
    """Generate and persist markdown report."""
    markdown = build_report_markdown(results)
    return save_report_file(markdown, output_dir=output_dir)


def run_and_save_report(output_dir=None):
    """Run all benchmarks and save report markdown."""
    results = run_all_benchmarks()
    return generate_comparison_report(results, output_dir=output_dir)


def print_usage():
    """Print usage information."""
    print(
        """
Bitmap Allocators Benchmark Suite with Report
==============================================

Usage:
  python run_report.py [OPTIONS]

Options:
  --help      - Show this help message

Output:
  Creates a markdown file named:
    report-YYYYMMDD-HHMMSS.md
"""
    )


if __name__ == "__main__":
    if "--help" in sys.argv or "-h" in sys.argv:
        print_usage()
        sys.exit(0)

    report_path = run_and_save_report()
    print(f"\nReport saved to: {report_path}")
