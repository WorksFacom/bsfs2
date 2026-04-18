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

    def find_free_block(self):
        """Find first free block via sequential scan - O(n)."""
        for idx, value in enumerate(self.bitmap):
            if value == 0:
                return idx
        return -1

    def allocate_blocks(self, count):
        """Allocate count free blocks using sequential scans - O(count × n)."""
        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            if idx == -1:
                for allocated_idx in allocated:
                    self.bitmap[allocated_idx] = 0
                return []
            self.bitmap[idx] = 1
            allocated.append(idx)

        return allocated

    def deallocate_blocks(self, indices):
        """Deallocate blocks back to the bitmap."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
        return True

    def count_free_blocks(self):
        """Count free blocks."""
        return self.bitmap.count(0)
