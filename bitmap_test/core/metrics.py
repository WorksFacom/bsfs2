"""Benchmark metrics aggregation helpers."""

from __future__ import annotations

from statistics import median


def _p95(values):
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[int(len(ordered) * 0.95)]


def build_stress_metrics(name, alloc_times_us, dealloc_times_us, bitmap_size, free_blocks):
    alloc_avg = sum(alloc_times_us) / len(alloc_times_us) if alloc_times_us else 0
    alloc_median = median(alloc_times_us) if alloc_times_us else 0
    alloc_p95 = _p95(alloc_times_us)

    dealloc_avg = sum(dealloc_times_us) / len(dealloc_times_us) if dealloc_times_us else 0
    dealloc_median = median(dealloc_times_us) if dealloc_times_us else 0
    dealloc_p95 = _p95(dealloc_times_us)

    total_time_us = sum(alloc_times_us) + sum(dealloc_times_us)
    total_time_s = total_time_us / 1e6
    total_ops = len(alloc_times_us) + len(dealloc_times_us)
    ops_per_sec = total_ops / total_time_s if total_time_s > 0 else 0
    avg_time_per_op = total_time_us / total_ops if total_ops > 0 else 0
    occupied = bitmap_size - free_blocks

    return {
        "name": name,
        "ops_per_sec": ops_per_sec,
        "total_time_s": total_time_s,
        "alloc_avg": alloc_avg,
        "alloc_median": alloc_median,
        "alloc_p95": alloc_p95,
        "dealloc_avg": dealloc_avg,
        "dealloc_median": dealloc_median,
        "dealloc_p95": dealloc_p95,
        "num_allocs": len(alloc_times_us),
        "num_deallocs": len(dealloc_times_us),
        "avg_time_per_op": avg_time_per_op,
        "occupied": occupied,
        "free_blocks": free_blocks,
    }
