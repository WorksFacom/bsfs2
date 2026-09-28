"""
Comparative report across all registered strategies.

`run_all_benchmarks` stresses every strategy through `BenchmarkRunner`. `build_report_markdown`
renders a throughput ranking, a detailed metrics table and the winner's margin over second
place. `save_report_file` writes it to `report-YYYYMMDD-HHMMSS.md`.
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from core import FIXED_BITMAP_SIZE_BLOCKS

from .benchmark_runner import BenchmarkRunner


class ReportRunner:
    @staticmethod
    def run_all_benchmarks(
        strategy_specs,
        bitmap_size=FIXED_BITMAP_SIZE_BLOCKS,
        blocks_per_alloc=2560,
        num_operations=100,
    ):
        results = []
        total = len(strategy_specs)

        print("\n" + "=" * 80)
        print("BITMAP ALLOCATORS - COMPREHENSIVE BENCHMARK SUITE")
        print("=" * 80)
        print(f"\nRunning benchmarks for all {total} strategies...\n")

        for i, (_, allocator_class, name) in enumerate(strategy_specs, 1):
            print(f"[{i}/{total}] Testing {name}...", end=" ", flush=True)
            runner = BenchmarkRunner(allocator_class, name, bitmap_size=bitmap_size)
            results.append(
                runner.run_stress(
                    blocks_per_alloc=blocks_per_alloc,
                    num_operations=num_operations,
                    show_progress=False,
                    show_summary=False,
                )
            )
            print("✓")

        return results

    @staticmethod
    def build_report_markdown(results):
        generated_at = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        sorted_results = sorted(results, key=lambda x: x["ops_per_sec"], reverse=True)
        winner = sorted_results[0]
        second = sorted_results[1] if len(sorted_results) > 1 else sorted_results[0]
        gain = (
            (winner["ops_per_sec"] / second["ops_per_sec"] - 1) * 100
            if second["ops_per_sec"] > 0
            else 0.0
        )

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
                "| Strategy | Ops/sec | Alloc avg (µs) | Dealloc avg (µs) | Fragmentation | Total time (s) |",
                "|---|---:|---:|---:|---:|---:|",
            ]
        )

        for result in results:
            lines.append(
                f"| {result['name']} | {result['ops_per_sec']:,.0f} | {result['alloc_avg']:.2f} | "
                f"{result['dealloc_avg']:.2f} | {result['fragmentation']:.4f} | {result['total_time_s']:.3f} |"
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

    @staticmethod
    def save_report_file(markdown, output_dir=None):
        directory = Path(output_dir) if output_dir else Path.cwd()
        directory.mkdir(parents=True, exist_ok=True)
        filename = f"report-{datetime.now().strftime('%Y%m%d-%H%M%S')}.md"
        output_path = directory / filename
        output_path.write_text(markdown, encoding="utf-8")
        return output_path.resolve()
