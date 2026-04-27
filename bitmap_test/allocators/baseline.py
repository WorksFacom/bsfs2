"""
Baseline sequential-scan implementation for bitmap block allocation.
"""

from core import BaseAllocator, register


@register("baseline", display_name="Baseline Sequential Scan")
class BitmapBaseline(BaseAllocator):
    """Baseline allocator using linear scan over the bitmap."""

    def __init__(self, size):
        """Initialize bitmap of size blocks, all free (0)."""
        super().__init__(size)

    def allocate_blocks(self, count):
        """Allocate count free blocks using single scan for contiguous runs - O(n)."""
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
        """Deallocate blocks back to the bitmap."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
        return True

    def count_free_blocks(self):
        """Count free blocks."""
        return self.free_blocks_remaining
