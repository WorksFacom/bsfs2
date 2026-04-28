"""
Pareto workload scenario with three file sizes.

File size distribution:
- 70% of operations use 1 MiB files (256 blocks at 4 KiB each).
- 25% use 10 MiB files (2,560 blocks).
- 5% use 1 GiB files (262,144 blocks).

Workload:
- each operation the RNG picks a file size then decides alloc/dealloc (80/20 split).
- each size class maintains its own allocated block list and separate timing lists.

Extra metrics:
- in addition to global metrics, returns `ops_per_sec_<size>`, `alloc_avg_<size>`, etc.
  for each size class (1mib, 10mib, 1gib), enabling per-file-class analysis.
"""

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
    SIZE_KEYS = {
        256: "1mib",
        2560: "10mib",
        262144: "1gib",
    }

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
        alloc_counts_by_size = {256: 0, 2560: 0, 262144: 0}
        dealloc_counts_by_size = {256: 0, 2560: 0, 262144: 0}
        alloc_times_by_size = {256: [], 2560: [], 262144: []}
        dealloc_times_by_size = {256: [], 2560: [], 262144: []}

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
                    elapsed_us = (t_end - t_start) / 1000.0
                    alloc_times.append(elapsed_us)
                    alloc_times_by_size[file_size].append(elapsed_us)
                    allocated[file_size].extend(allocated_blocks)
                    alloc_counts_by_size[file_size] += 1
            else:
                free_count = min(file_size, len(allocated[file_size]))
                if free_count > 0:
                    blocks_to_free = rng.sample(allocated[file_size], free_count)
                    t_start = time.perf_counter_ns()
                    success = allocator.deallocate_blocks(blocks_to_free)
                    t_end = time.perf_counter_ns()

                    if success:
                        elapsed_us = (t_end - t_start) / 1000.0
                        dealloc_times.append(elapsed_us)
                        dealloc_times_by_size[file_size].append(elapsed_us)
                        freed = set(blocks_to_free)
                        allocated[file_size] = [b for b in allocated[file_size] if b not in freed]
                        dealloc_counts_by_size[file_size] += 1

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
        for size_blocks, size_key in self.SIZE_KEYS.items():
            alloc_size_times = alloc_times_by_size[size_blocks]
            dealloc_size_times = dealloc_times_by_size[size_blocks]
            total_time_us = sum(alloc_size_times) + sum(dealloc_size_times)
            total_time_s = total_time_us / 1e6
            total_ops = alloc_counts_by_size[size_blocks] + dealloc_counts_by_size[size_blocks]
            ops_per_sec = total_ops / total_time_s if total_time_s > 0 else 0.0
            alloc_avg = sum(alloc_size_times) / len(alloc_size_times) if alloc_size_times else 0.0
            dealloc_avg = sum(dealloc_size_times) / len(dealloc_size_times) if dealloc_size_times else 0.0

            result[f"alloc_{size_key}"] = alloc_counts_by_size[size_blocks]
            result[f"dealloc_{size_key}"] = dealloc_counts_by_size[size_blocks]
            result[f"ops_per_sec_{size_key}"] = ops_per_sec
            result[f"total_time_s_{size_key}"] = total_time_s
            result[f"alloc_avg_{size_key}"] = alloc_avg
            result[f"dealloc_avg_{size_key}"] = dealloc_avg
        return result
