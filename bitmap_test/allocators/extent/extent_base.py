"""
Shared policy for extent-first allocators.

`RangeExtentAllocator` models free space as disjoint ranges and implements the
whole allocation policy here, delegating only the data-structure operations to
subclasses. This keeps the concrete allocators (treap / red-black / B+tree of
free ranges) differing *only* by their underlying structure, which makes them
directly comparable and lets them cross-validate against each other (same policy
=> identical resulting bitmap).

Policy:
- Allocation is extent-first with a lowest-address scatter fallback:
  1. best-fit: find the smallest single contiguous range with size >= count;
     if one exists, carve `count` blocks off its front and reinsert the
     remainder. This returns a contiguous extent.
  2. fallback: if no single range fits, repeatedly take from the lowest-address
     range until `count` blocks are gathered (non-contiguous). This keeps low
     addresses dense (locality) and is cheaper than a largest-range scan.
  All-or-nothing: a partial fallback is rolled back on exhaustion.
- Deallocation marks blocks free, groups contiguous indices into ranges, and
  reinserts each range merging with adjacent neighbors (coalescing).

This differs from `avl_range_extent`, which uses a *largest-range* fallback.

Subclasses implement the structure hooks:
- `_init_structure(size)`  : seed the structure with one range [0, size-1].
- `_find_best_fit(count)`  : smallest range with size >= count, or None (no removal).
- `_pop_min_range()`       : remove and return the lowest-`start` range, or None.
- `_insert_range(start, end)`
- `_delete_range(start)`   : remove the range whose key is `start`.
- `_predecessor_range(key)`: range with the greatest start < key, or None.
- `_successor_range(key)`  : range with the smallest start > key, or None.

Each hook that returns a range yields a `(start, end)` tuple (inclusive).
"""

from core import BaseAllocator


class RangeExtentAllocator(BaseAllocator):
    """Base class implementing the extent-first allocation policy."""

    # Same best-fit + lowest-address-scatter policy across treap/rb/bplustree,
    # so they must produce byte-identical bitmaps on a fixed workload.
    CONSISTENCY_GROUP = "extent_bestfit_lowaddr"

    def __init__(self, size):
        super().__init__(size)
        self._init_structure(size)

    # ------------------------------------------------------------------
    # Allocation policy
    # ------------------------------------------------------------------

    def allocate_blocks(self, count):
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        # Step 1: best-fit single contiguous extent.
        best = self._find_best_fit(count)
        if best is not None:
            start, end = best
            self._delete_range(start)
            if start + count <= end:
                self._insert_range(start + count, end)
            allocated = list(range(start, start + count))
            for idx in allocated:
                self.bitmap[idx] = 1
            return allocated

        # Step 2: fallback - scatter from the lowest-address ranges.
        allocated = []
        remaining = count
        while remaining > 0:
            rng = self._pop_min_range()
            if rng is None:
                self.rollback_bitmap_allocation(allocated)
                for idx in allocated:
                    self._add_free_range(idx, idx)
                return []
            start, end = rng
            take = min(remaining, end - start + 1)
            allocated.extend(range(start, start + take))
            if start + take <= end:
                self._insert_range(start + take, end)
            remaining -= take

        for idx in allocated:
            self.bitmap[idx] = 1
        return allocated

    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0

        sorted_indices = sorted(indices)
        range_start = sorted_indices[0]
        range_end = range_start
        for idx in sorted_indices[1:]:
            if idx == range_end + 1:
                range_end = idx
                continue
            self._add_free_range(range_start, range_end)
            range_start = idx
            range_end = idx
        self._add_free_range(range_start, range_end)
        return True

    def count_free_blocks(self):
        return self.free_blocks_remaining

    # ------------------------------------------------------------------
    # Generic range bookkeeping (uses structure hooks)
    # ------------------------------------------------------------------

    def _add_free_range(self, start, end):
        """Insert a free range, coalescing with adjacent neighbors."""
        if start > end:
            return

        while True:
            pred = self._predecessor_range(start)
            if pred and pred[1] + 1 >= start:
                start = min(start, pred[0])
                end = max(end, pred[1])
                self._delete_range(pred[0])
                continue
            break

        while True:
            succ = self._successor_range(end)
            if succ and succ[0] <= end + 1:
                end = max(end, succ[1])
                self._delete_range(succ[0])
                continue
            break

        self._insert_range(start, end)

    # ------------------------------------------------------------------
    # Structure hooks (implemented by subclasses)
    # ------------------------------------------------------------------

    def _init_structure(self, size):
        raise NotImplementedError

    def _find_best_fit(self, count):
        raise NotImplementedError

    def _pop_min_range(self):
        raise NotImplementedError

    def _insert_range(self, start, end):
        raise NotImplementedError

    def _delete_range(self, start):
        raise NotImplementedError

    def _predecessor_range(self, key):
        raise NotImplementedError

    def _successor_range(self, key):
        raise NotImplementedError
