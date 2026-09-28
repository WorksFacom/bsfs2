"""
Lazy min-heap allocator.

Recycled indices go into a min-heap. `next_fresh` covers never-used blocks, so the heap never has
to be preloaded with every free block. Allocation takes the smaller of heap top and `next_fresh`,
popping or advancing. Frees push onto the heap and never move the cursor.
"""

import heapq

from core import BaseAllocator, register


@register("heap", display_name="Min-Heap")
class BitmapHeap(BaseAllocator):
    def __init__(self, size):
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
        """O(count log n)."""
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
        """O(count log n)."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            heapq.heappush(self.heap, idx)
        return True
    
    def count_free_blocks(self):
        return self.free_blocks_remaining
