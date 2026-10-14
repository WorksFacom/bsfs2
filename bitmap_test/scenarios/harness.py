"""
Bookkeeping shared by the statistical scenarios.

The scenario tracks allocated blocks in an `array('q')` (8 B per block instead of a 36 B int object
plus pointer), so long runs near a full 250 GiB disk stay in memory and the GC has nothing to scan.
Deallocs sample positions and swap-remove them in O(k) instead of rebuilding the list.
`random.sample(range(n), k)` draws the same positions `random.sample(seq, k)` would.

GC stays off while a scenario runs, like `timeit`, so collector passes over harness and allocator
objects don't land inside a timed call. Callers collect between runs.

The deep memory walk is O(objects), so the mixed scenarios sample it right after deallocs, where
the lazy and range structures peak (an alloc adds at most one split range), on every dealloc for
short runs and about 200 times for long ones.
"""

from __future__ import annotations

import gc
from array import array
from contextlib import contextmanager


def new_block_list():
    return array("q")


def remove_positions(blocks, positions):
    # descending, so the tail element moved into a hole is never one still pending removal
    for pos in sorted(positions, reverse=True):
        last = blocks.pop()
        if pos < len(blocks):
            blocks[pos] = last


def memory_sample_every(num_operations):
    return 1 if num_operations <= 1000 else max(1, num_operations // 1000)


@contextmanager
def gc_paused():
    was_enabled = gc.isenabled()
    gc.disable()
    try:
        yield
    finally:
        if was_enabled:
            gc.enable()
