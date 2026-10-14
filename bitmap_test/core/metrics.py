"""
Metrics aggregation for benchmark results.

`build_stress_metrics` turns per-op alloc/dealloc times (us) into ops/sec, averages, counts
and final bitmap state. Fragmentation is `(occupied - runs) / occupied`, where `runs` is the
number of contiguous occupied runs. Closer to 1 is more contiguous; an empty bitmap is 0.0.

`measure_allocator_memory_kib` walks every object reachable from the allocator (gc referents), so
node-based trees are counted in full instead of just their root.
"""

from __future__ import annotations

import gc
import sys
import types


def _calculate_fragmentation(bitmap):
    """(occupied - runs) / occupied. total_frags counts occupied blocks, total_files counts runs."""
    if not bitmap or bitmap.count(1) == 0:
        return 0.0

    data = getattr(bitmap, "_data", None)
    if data is not None:
        # same counts as the loop below, at C speed: a run starts at every 0->1 edge plus index 0
        total_frags = bitmap.count(1)
        total_files = data.count(b"\x00\x01") + (1 if data[0] == 1 else 0)
        return (total_frags - total_files) / total_frags

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


_ATOMIC = (int, float, complex, str, bytes, bool, type(None))
_NOT_DATA = (type, types.ModuleType, types.FunctionType, types.BuiltinFunctionType, types.MethodType)
_MANAGED_DICT = 1 << 4  # Py_TPFLAGS_MANAGED_DICT


def _object_bytes(obj, n_referents):
    size = sys.getsizeof(obj)
    # getsizeof skips the inline attribute values of plain (non-__slots__) instances, about 8 B per
    # attribute plus a 24 B header on 3.12+
    if type(obj).__flags__ & _MANAGED_DICT:
        size += 8 * max(0, n_referents - 1) + 24
    return size


def measure_allocator_memory_kib(allocator):
    """Deep size of everything reachable from the allocator, bitmap excluded, shared objects once."""
    seen = {id(allocator)}
    bitmap = getattr(allocator, "bitmap", None)
    if bitmap is not None:
        seen.add(id(bitmap))
    total = 0
    stack = list(gc.get_referents(allocator))
    while stack:
        obj = stack.pop()
        oid = id(obj)
        if oid in seen or isinstance(obj, _NOT_DATA):
            continue
        seen.add(oid)
        if isinstance(obj, _ATOMIC):
            total += sys.getsizeof(obj)
            continue
        refs = gc.get_referents(obj)
        total += _object_bytes(obj, len(refs))
        stack.extend(refs)
    return total / 1024.0
