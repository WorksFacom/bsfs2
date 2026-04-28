"""
Allocation-only workload scenario with no deallocations.

Workload:
- performs `num_operations` consecutive allocations of `blocks_per_alloc` blocks each.
- the bitmap fills progressively with no blocks ever freed.
- useful for measuring pure allocation throughput and behaviour under high occupancy.

Metrics:
- `dealloc_avg` is always 0; `fragmentation` tends toward 0 since blocks are allocated contiguously.
"""

from __future__ import annotations

import time

from core.metrics import build_stress_metrics

from .base import BenchmarkScenario


class AllocationOnlyScenario(BenchmarkScenario):
    def __init__(self, blocks_per_alloc, num_operations):
        self.blocks_per_alloc = blocks_per_alloc
        self.num_operations = num_operations

    def run(
        self,
        allocator_class,
        allocator_name,
        bitmap_size,
        show_progress=False,
        show_summary=False,
    ):
        allocator = allocator_class(bitmap_size)
        alloc_times = []

        progress_step = max(1, self.num_operations // 10)
        for i in range(self.num_operations):
            if show_progress and i % progress_step == 0 and i > 0:
                print(
                    f"\rProgress: {i:,}/{self.num_operations:,} ({100 * i // self.num_operations}%)\033[K",
                    end="",
                    flush=True,
                )

            t_start = time.perf_counter_ns()
            blocks = allocator.allocate_blocks(self.blocks_per_alloc)
            t_end = time.perf_counter_ns()

            if blocks:
                alloc_times.append((t_end - t_start) / 1000.0)

        if show_progress:
            print(
                f"\rProgress: {self.num_operations:,}/{self.num_operations:,} (100%)\033[K"
            )

        result = build_stress_metrics(
            name=f"{allocator_name} (allocation_only)",
            alloc_times_us=alloc_times,
            dealloc_times_us=[],
            bitmap_size=bitmap_size,
            free_blocks=allocator.count_free_blocks(),
            bitmap=allocator.bitmap,
        )
        result["mode"] = "allocation_only"
        return result
