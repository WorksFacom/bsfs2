"""
Metrics aggregation for benchmark results.

`build_stress_metrics` turns per-op alloc/dealloc times (us) into ops/sec, averages, counts
and final bitmap state. Fragmentation is `(occupied - runs) / occupied`, where `runs` is the
number of contiguous occupied runs. Closer to 1 is more contiguous; an empty bitmap is 0.0.
"""

from __future__ import annotations

import sys


def _calculate_fragmentation(bitmap):
    """(occupied - runs) / occupied. total_frags counts occupied blocks, total_files counts runs."""
    if not bitmap or bitmap.count(1) == 0:
        return 0.0

    total_files = 0
    total_frags = 0
    in_fragment = False

    for bit in bitmap:
        if bit == 1:
            if not in_fragment:
                total_files += 1
                in_fragment = True
            total_frags += 1
        else:
            in_fragment = False

    if total_frags == 0:
        return 0.0

    return (total_frags - total_files) / total_frags


def build_stress_metrics(name, alloc_times_us, dealloc_times_us, bitmap_size, free_blocks, bitmap=None):
    alloc_avg = sum(alloc_times_us) / len(alloc_times_us) if alloc_times_us else 0
    dealloc_avg = sum(dealloc_times_us) / len(dealloc_times_us) if dealloc_times_us else 0

    total_time_us = sum(alloc_times_us) + sum(dealloc_times_us)
    total_time_s = total_time_us / 1e6
    total_ops = len(alloc_times_us) + len(dealloc_times_us)
    ops_per_sec = total_ops / total_time_s if total_time_s > 0 else 0
    avg_time_per_op = total_time_us / total_ops if total_ops > 0 else 0
    occupied = bitmap_size - free_blocks
    fragmentation = _calculate_fragmentation(bitmap) if bitmap else 0.0

    return {
        "name": name,
        "ops_per_sec": ops_per_sec,
        "total_time_s": total_time_s,
        "alloc_avg": alloc_avg,
        "dealloc_avg": dealloc_avg,
        "num_allocs": len(alloc_times_us),
        "num_deallocs": len(dealloc_times_us),
        "avg_time_per_op": avg_time_per_op,
        "occupied": occupied,
        "free_blocks": free_blocks,
        "fragmentation": fragmentation,
    }


def measure_allocator_memory_kib(allocator):
    """Approximate aux memory of the allocator's structures via sys.getsizeof, bitmap excluded."""
    SKIP = {"bitmap", "size", "free_blocks_remaining"}
    total = 0
    for attr, val in vars(allocator).items():
        if attr in SKIP:
            continue
        total += sys.getsizeof(val)
        if isinstance(val, dict):
            for k, v in val.items():
                total += sys.getsizeof(k) + sys.getsizeof(v)
        elif isinstance(val, list):
            for item in val:
                total += sys.getsizeof(item)
    return total / 1024.0
