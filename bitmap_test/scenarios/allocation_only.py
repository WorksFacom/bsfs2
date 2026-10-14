"""
Allocation-only workload.

`num_operations` allocations of `blocks_per_alloc` blocks each, nothing ever freed, so the bitmap
just fills up. Measures raw allocation throughput and behavior near full. `dealloc_avg` is always
0 and fragmentation stays low since blocks come out contiguous.
"""

from __future__ import annotations

import time

from core.metrics import build_stress_metrics, measure_allocator_memory_kib

from .base import BenchmarkScenario
from .harness import gc_paused, memory_sample_every


class AllocationOnlyScenario(BenchmarkScenario):
    def __init__(self, blocks_per_alloc, num_operations):
        self.blocks_per_alloc = blocks_per_alloc
        self.num_operations = num_operations

    def run(self, allocator_class, allocator_name, bitmap_size, show_progress=False, show_summary=False):
        with gc_paused():
            return self._run(allocator_class, allocator_name, bitmap_size, show_progress)

    def _run(self, allocator_class, allocator_name, bitmap_size, show_progress):
        allocator = allocator_class(bitmap_size)
        alloc_times = []
        peak_mem = 0.0
        mem_every = memory_sample_every(self.num_operations)

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
            if i % mem_every == 0 or i == self.num_operations - 1:
                peak_mem = max(peak_mem, measure_allocator_memory_kib(allocator))

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
        result["peak_memory_kib"] = peak_mem
        return result
