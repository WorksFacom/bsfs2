"""
Pareto workload with three file sizes.

Sizes: 70% 1 MiB (256 blocks), 25% 10 MiB (2,560), 5% 1 GiB (262,144). Each op picks a size,
then alloc or dealloc 80/20; a dealloc frees a random sample of that class's blocks, not a whole
file. Every size class keeps its own allocated blocks and timings and adds
`ops_per_sec_<size>`, `alloc_avg_<size>` etc. (1mib, 10mib, 1gib) on top of the global metrics.
"""

from __future__ import annotations

import random
import time

from core.metrics import build_stress_metrics, measure_allocator_memory_kib

from .base import BenchmarkScenario
from .harness import gc_paused, memory_sample_every, new_block_list, remove_positions


class MultisizeParetoScenario(BenchmarkScenario):
    FILE_SIZES = [
        (0.70, 256),      # 1 MiB, 4 KiB blocks
        (0.25, 2560),     # 10 MiB
        (0.05, 262144),   # 1 GiB
    ]
    SIZE_KEYS = {
        256: "1mib",
        2560: "10mib",
        262144: "1gib",
    }

    def __init__(self, num_operations, seed):
        self.num_operations = num_operations
        self.seed = seed

    def run(self, allocator_class, allocator_name, bitmap_size, show_progress=False, show_summary=False):
        with gc_paused():
            return self._run(allocator_class, allocator_name, bitmap_size, show_progress)

    def _run(self, allocator_class, allocator_name, bitmap_size, show_progress):
        allocator = allocator_class(bitmap_size)
        rng = random.Random(self.seed)

        alloc_times = []
        dealloc_times = []
        allocated = {256: new_block_list(), 2560: new_block_list(), 262144: new_block_list()}
        alloc_counts_by_size = {256: 0, 2560: 0, 262144: 0}
        dealloc_counts_by_size = {256: 0, 2560: 0, 262144: 0}
        alloc_times_by_size = {256: [], 2560: [], 262144: []}
        dealloc_times_by_size = {256: [], 2560: [], 262144: []}
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

            freed_now = False
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
                    positions = rng.sample(range(len(allocated[file_size])), free_count)
                    blocks_to_free = [allocated[file_size][p] for p in positions]
                    t_start = time.perf_counter_ns()
                    success = allocator.deallocate_blocks(blocks_to_free)
                    t_end = time.perf_counter_ns()

                    if success:
                        elapsed_us = (t_end - t_start) / 1000.0
                        dealloc_times.append(elapsed_us)
                        freed_now = True
                        dealloc_times_by_size[file_size].append(elapsed_us)
                        remove_positions(allocated[file_size], positions)
                        dealloc_counts_by_size[file_size] += 1

            if (freed_now and len(dealloc_times) % mem_every == 0) or i == self.num_operations - 1:
                peak_mem = max(peak_mem, measure_allocator_memory_kib(allocator))

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
        result["peak_memory_kib"] = peak_mem
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
