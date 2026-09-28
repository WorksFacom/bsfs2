"""
Shared policy for extent-first allocators.

`RangeExtentAllocator` models free space as disjoint ranges and owns the whole policy; subclasses
only supply the data structure. So the treap, red-black and B+tree allocators differ only in
structure, and the shared policy means they must produce identical bitmaps, which lets them
cross-validate.

Allocation:
  1. best-fit: the smallest range with size >= count. Carve `count` blocks off its front and
     reinsert the rest. Result is contiguous.
  2. fallback: if nothing fits, keep taking from the lowest-address range until `count` blocks are
     gathered. Keeps low addresses dense and is cheaper than hunting for the largest range.
  All-or-nothing: a partial fallback is rolled back when space runs out.
Deallocation marks blocks free, groups contiguous indices into ranges and reinserts each one,
coalescing with neighbors.

`avl_range_extent` is the odd one out, it falls back to the *largest* range.

Structure hooks, ranges are inclusive `(start, end)` tuples:
- `_init_structure(size)`: seed with one range [0, size-1].
- `_find_best_fit(count)`: smallest range with size >= count, or None. Doesn't remove it.
- `_pop_min_range()`: remove and return the lowest-`start` range, or None.
- `_insert_range(start, end)`
- `_delete_range(start)`: remove the range keyed by `start`.
- `_predecessor_range(key)`: range with the greatest start < key, or None.
- `_successor_range(key)`: range with the smallest start > key, or None.
"""

from core import BaseAllocator


class RangeExtentAllocator(BaseAllocator):
    """Base class implementing the extent-first allocation policy."""

    # same best-fit + lowest-address scatter policy for treap/rb/bplustree, so they must produce
    # byte-identical bitmaps on a fixed workload
    CONSISTENCY_GROUP = "extent_bestfit_lowaddr"

    def __init__(self, size):
        super().__init__(size)
        self._init_structure(size)

    # ------------------------------------------------------------------
    # allocation policy
    # ------------------------------------------------------------------

    def allocate_blocks(self, count):
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        # best-fit, single contiguous extent
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

        # fallback, scatter from the lowest-address ranges
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
    # range bookkeeping on top of the structure hooks
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
    # structure hooks, subclasses implement these
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
