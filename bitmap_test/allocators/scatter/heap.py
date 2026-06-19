"""
Allocator with a lazy model using a min-heap for recycled blocks.

Strategy idea:
- use a min-heap for fast access to the smallest recycled block.
- combine heap with fresh range (`next_fresh`) to avoid preloading all free
  blocks into the heap.

How `next_fresh` works:
- points to the smallest index never allocated.
- when the selected minimum free block is fresh, the cursor advances.
- deallocated blocks do not return to fresh range; they go into the heap.

Allocation:
- picks the smallest free index between heap top (recycled) and
  `next_fresh` (never-used blocks).
- when using `next_fresh`, advances cursor; when using recycled block, pops
  from heap.
- marks each selected index as occupied in the bitmap.

Deallocation:
- validates indices, marks blocks as free in the bitmap, and pushes indices
  into the min-heap for reuse.
"""

import heapq

from core import BaseAllocator, register


@register("heap", display_name="Min-Heap")
class BitmapHeap(BaseAllocator):
    """Min-Heap indexed Bitmap Allocator"""
    
    def __init__(self, size):
        """Initialize lazy free-tracking structures."""
        super().__init__(size)
        self.heap = []
        self.next_fresh = 0
    
    def _pop_min_free(self):
        if self.next_fresh >= self.size and not self.heap:
            return -1
        if not self.heap:
            idx = self.next_fresh
            self.next_fresh += 1
            return idx
        if self.next_fresh >= self.size or self.heap[0] < self.next_fresh:
            return heapq.heappop(self.heap)
        idx = self.next_fresh
        self.next_fresh += 1
        return idx
    
    def allocate_blocks(self, count):
        """Allocate count free blocks - O(count × log n)"""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self._pop_min_free()
            self.bitmap[idx] = 1
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks - O(count × log n)"""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            heapq.heappush(self.heap, idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks"""
        return self.free_blocks_remaining
