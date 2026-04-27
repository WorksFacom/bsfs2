"""
Segment Tree allocator for bitmap block management.

For very large bitmaps, this implementation uses a sparse/lazy model:
- fresh free space is represented by a monotonic cursor (`next_fresh`)
- recycled free blocks are tracked in a min-heap

This keeps memory bounded by touched blocks instead of bitmap size.
"""

from __future__ import annotations

import heapq

from core import BaseAllocator, register


@register("segtree", display_name="Segment Tree")
class BitmapSegmentTree(BaseAllocator):
    """Bitmap allocator using sparse free-range tracking."""

    def __init__(self, size):
        super().__init__(size)
        self.next_fresh = 0
        self.recycled_heap = []

    def _pop_min_free(self):
        if self.next_fresh >= self.size and not self.recycled_heap:
            return -1
        if not self.recycled_heap:
            idx = self.next_fresh
            self.next_fresh += 1
            return idx
        if self.next_fresh >= self.size or self.recycled_heap[0] < self.next_fresh:
            return heapq.heappop(self.recycled_heap)
        idx = self.next_fresh
        self.next_fresh += 1
        return idx

    def allocate_blocks(self, count):
        """Allocate count blocks."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []
        for _ in range(count):
            idx = self._pop_min_free()
            if idx == -1:
                return []
            self.bitmap[idx] = 1
            allocated.append(idx)
        return allocated

    def deallocate_blocks(self, indices):
        """Deallocate blocks."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            heapq.heappush(self.recycled_heap, idx)
        return True

    def count_free_blocks(self):
        """Count total free blocks."""
        return self.free_blocks_remaining
