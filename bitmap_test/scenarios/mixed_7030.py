"""Mixed workload scenario with 70% allocation and 30% deallocation."""

from __future__ import annotations

import random
import time

from core.metrics import build_stress_metrics

from .base import BenchmarkScenario


class Mixed7030Scenario(BenchmarkScenario):
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

        progress_step = max(1, self.num_operations // 10)
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

        result = build_stress_metrics(
            name=f"{allocator_name} (mixed_7030)",
            alloc_times_us=alloc_times,
            dealloc_times_us=dealloc_times,
            bitmap_size=bitmap_size,
            free_blocks=allocator.count_free_blocks(),
            bitmap=allocator.bitmap,
        )
        result["mode"] = "mixed_7030"
        return result
