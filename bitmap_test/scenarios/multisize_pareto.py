"""Multisize workload scenario with Pareto distribution (80% alloc / 20% dealloc)."""

from __future__ import annotations

import random
import time

from core.metrics import build_stress_metrics

from .base import BenchmarkScenario


class MultisizeParetoScenario(BenchmarkScenario):
    FILE_SIZES = [
        (0.70, 256),      # 1 MB → 256 blocks (block = 4 KiB)
        (0.25, 2560),     # 10 MB → 2560 blocks
        (0.05, 262144),   # 1 GB → 262144 blocks
    ]

    def __init__(self, num_operations, seed):
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
        allocated = {256: [], 2560: [], 262144: []}

        progress_step = max(1, self.num_operations // 10)
        for i in range(self.num_operations):
            if show_progress and i % progress_step == 0 and i > 0:
                print(
                    f"\rProgress: {i:,}/{self.num_operations:,} ({100 * i // self.num_operations}%)\033[K",
                    end="",
                    flush=True,
                )

            r = rng.random()
            if r < 0.70:
                file_size = 256
            elif r < 0.95:
                file_size = 2560
            else:
                file_size = 262144

            estimated_free_blocks = bitmap_size - sum(len(blocks) for blocks in allocated.values())
            should_alloc = rng.random() < 0.80 or not allocated[file_size]

            if estimated_free_blocks <= file_size and allocated[file_size]:
                should_alloc = False

            if should_alloc:
                t_start = time.perf_counter_ns()
                allocated_blocks = allocator.allocate_blocks(file_size)
                t_end = time.perf_counter_ns()

                if allocated_blocks:
                    alloc_times.append((t_end - t_start) / 1000.0)
                    allocated[file_size].extend(allocated_blocks)
            else:
                free_count = min(file_size, len(allocated[file_size]))
                if free_count > 0:
                    blocks_to_free = rng.sample(allocated[file_size], free_count)
                    t_start = time.perf_counter_ns()
                    success = allocator.deallocate_blocks(blocks_to_free)
                    t_end = time.perf_counter_ns()

                    if success:
                        dealloc_times.append((t_end - t_start) / 1000.0)
                        freed = set(blocks_to_free)
                        allocated[file_size] = [b for b in allocated[file_size] if b not in freed]

        if show_progress:
            print(
                f"\rProgress: {self.num_operations:,}/{self.num_operations:,} (100%)\033[K"
            )

        result = build_stress_metrics(
            name=f"{allocator_name} (multisize_pareto)",
            alloc_times_us=alloc_times,
            dealloc_times_us=dealloc_times,
            bitmap_size=bitmap_size,
            free_blocks=allocator.count_free_blocks(),
            bitmap=allocator.bitmap,
        )
        result["mode"] = "multisize_pareto"
        return result
