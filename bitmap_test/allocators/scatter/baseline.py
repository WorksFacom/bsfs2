"""
Baseline allocator: linear scan over the bitmap, no index at all.

Allocation scans from 0 and collects the first `count` free blocks, contiguous or not, and only
marks them once it has enough. Deallocation validates and clears the bits.
"""

from core import BaseAllocator, register


@register("baseline", display_name="Baseline Sequential Scan")
class BitmapBaseline(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)

    def allocate_blocks(self, count):
        """Single O(n) scan."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []
        for i in range(self.size):
            if self.bitmap[i] == 0:
                allocated.append(i)
                if len(allocated) == count:
                    for idx in allocated:
                        self.bitmap[idx] = 1
                    return allocated

        return []

    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
        return True

    def count_free_blocks(self):
        return self.free_blocks_remaining
