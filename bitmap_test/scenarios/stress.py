"""
Default random mixed workload scenario: 70% allocation / 30% deallocation.

Workload:
- each operation the RNG decides with probability 0.7 whether to allocate or deallocate.
- deallocations randomly select blocks from the currently allocated list.
- when the bitmap is nearly full, deallocation is forced to keep the cycle going.

Usage:
- used by `BenchmarkRunner.run_stress` with a fixed seed of 42 and `blocks_per_alloc=2560`.
- `show_summary=True` prints a detailed results table; `show_progress=True` shows a progress indicator.
"""

from __future__ import annotations

import random
import time

from core.metrics import build_stress_metrics

from .base import BenchmarkScenario


class StressScenario(BenchmarkScenario):
    def __init__(self, blocks_per_alloc=5, num_operations=100000, seed=42):
        self.blocks_per_alloc = blocks_per_alloc
        self.num_operations = num_operations
        self.seed = seed

    def run(
        self,
        allocator_class,
        allocator_name,
        bitmap_size,
        show_progress=True,
        show_summary=True,
    ):
        if show_summary:
            print(f"\n{'=' * 74}")
            print(f"STRESS TEST: {allocator_name} Multiple Block Allocation Performance")
            print("=" * 74)

            print("\nConfiguration:")
            print(f"  Bitmap size: {bitmap_size:,} blocks")
            print(f"  Blocks per allocation: {self.blocks_per_alloc}")
            print(f"  Total operations: {self.num_operations:,}")
            print("  Operation mix: 70% allocate / 30% deallocate")
            print("  Deallocation policy: random blocks among allocated")

        allocator = allocator_class(bitmap_size)
        rng = random.Random(self.seed)

        alloc_times = []
        dealloc_times = []
        allocated_blocks = []
        progress_step = max(1, self.num_operations // 10)

        if show_progress:
            print("")

        for i in range(self.num_operations):
            if show_progress and i % progress_step == 0 and i > 0:
                print(
                    f"\rProgress: {i:,}/{self.num_operations:,} ({100 * i // self.num_operations}%)\033[K",
                    end="",
                    flush=True,
                )

            estimated_free_blocks = bitmap_size - len(allocated_blocks)
            should_allocate = rng.random() < 0.7 or not allocated_blocks

            if estimated_free_blocks <= self.blocks_per_alloc and allocated_blocks:
                should_allocate = False

            if should_allocate:
                t_start = time.perf_counter_ns()
                allocated = allocator.allocate_blocks(self.blocks_per_alloc)
                t_end = time.perf_counter_ns()

                if allocated:
                    alloc_times.append((t_end - t_start) / 1000.0)
                    allocated_blocks.extend(allocated)
            else:
                free_count = min(self.blocks_per_alloc, len(allocated_blocks))
                blocks_to_free = rng.sample(allocated_blocks, free_count)
                t_start = time.perf_counter_ns()
                success = allocator.deallocate_blocks(blocks_to_free)
                t_end = time.perf_counter_ns()

                if success:
                    dealloc_times.append((t_end - t_start) / 1000.0)
                    freed = set(blocks_to_free)
                    allocated_blocks = [b for b in allocated_blocks if b not in freed]

        if show_progress:
            print(
                f"\rProgress: {self.num_operations:,}/{self.num_operations:,} (100%)\033[K"
            )

        free_blocks = allocator.count_free_blocks()
        results = build_stress_metrics(
            name=allocator_name,
            alloc_times_us=alloc_times,
            dealloc_times_us=dealloc_times,
            bitmap_size=bitmap_size,
            free_blocks=free_blocks,
            bitmap=allocator.bitmap,
        )

        if show_summary:
            print(f"\n{'=' * 74}")
            print("RESULTS")
            print("=" * 74)

            print("\nExecution:")
            print(f"  Total time: {results['total_time_s']:.3f}s")
            print(f"  Operations/sec: {results['ops_per_sec']:,.0f}")
            print(f"  Time per operation: {results['avg_time_per_op']:.3f} µs")

            print(f"\nAllocation ({self.blocks_per_alloc} blocks):")
            print(f"  Average: {results['alloc_avg']:.3f} µs")
            print(f"  Total calls: {results['num_allocs']:,}")

            print(f"\nDeallocation ({self.blocks_per_alloc} blocks):")
            print(f"  Average: {results['dealloc_avg']:.3f} µs")
            print(f"  Total calls: {results['num_deallocs']:,}")

            print("\nFinal state:")
            print(f"  Occupied blocks: {results['occupied']:,}")
            print(f"  Free blocks: {results['free_blocks']:,}")
            print(f"  Fragmentation: {results['fragmentation']:.2%}")

        return results
