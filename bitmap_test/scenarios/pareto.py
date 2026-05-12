"""
Pareto workload scenario: 80% allocation / 20% deallocation.

Workload:
- each operation the RNG decides with probability 0.8 whether to allocate or deallocate.
- allocated blocks are tracked in a list; deallocations pick a random sample from that list.
- when the bitmap is nearly full (free blocks ≤ `blocks_per_alloc`), deallocation is forced.

Parameters:
- `blocks_per_alloc`: fixed block count for both allocations and deallocations.
- `num_operations`: total operation count (alloc + dealloc mix).
- `seed`: RNG seed for reproducibility across strategies.
"""

from __future__ import annotations

import random
import time

from core.metrics import build_stress_metrics, measure_allocator_memory_kib

from .base import BenchmarkScenario


class ParetoScenario(BenchmarkScenario):
    def __init__(self, blocks_per_alloc, num_operations, seed):
        self.blocks_per_alloc = blocks_per_alloc
        self.num_operations = num_operations
        self.seed = seed

    def run(
        self,
        allocator_class,
        allocator_name,
        bitmap_size,
        show_progress=False,
        show_summary=False,
    ):
        allocator = allocator_class(bitmap_size)
        rng = random.Random(self.seed)
        alloc_times = []
        dealloc_times = []
        allocated_blocks = []
        peak_mem = 0.0

        progress_step = max(1, self.num_operations // 10)
        for i in range(self.num_operations):
            if show_progress and i % progress_step == 0 and i > 0:
                print(
                    f"\rProgress: {i:,}/{self.num_operations:,} ({100 * i // self.num_operations}%)\033[K",
                    end="",
                    flush=True,
                )

            estimated_free_blocks = bitmap_size - len(allocated_blocks)
            should_allocate = rng.random() < 0.8 or not allocated_blocks
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

            peak_mem = max(peak_mem, measure_allocator_memory_kib(allocator))

        if show_progress:
            print(
                f"\rProgress: {self.num_operations:,}/{self.num_operations:,} (100%)\033[K"
            )

        result = build_stress_metrics(
            name=f"{allocator_name} (pareto)",
            alloc_times_us=alloc_times,
            dealloc_times_us=dealloc_times,
            bitmap_size=bitmap_size,
            free_blocks=allocator.count_free_blocks(),
            bitmap=allocator.bitmap,
        )
        result["mode"] = "pareto"
        result["peak_memory_kib"] = peak_mem
        return result
