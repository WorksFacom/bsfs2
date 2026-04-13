"""
Baseline sequential-scan implementation for bitmap block allocation.
"""


class BitmapBaseline:
    """Baseline allocator using linear scan over the bitmap."""

    def __init__(self, size):
        """Initialize bitmap of size blocks, all free (0)."""
        self.bitmap = [0] * size
        self.size = size

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
        for idx in indices:
            if idx < 0 or idx >= self.size:
                return False
            if self.bitmap[idx] == 0:
                return False

        for idx in indices:
            self.bitmap[idx] = 0
        return True

    def count_free_blocks(self):
        """Count free blocks."""
        return self.bitmap.count(0)
