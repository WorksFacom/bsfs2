"""
Sequential allocator with a first-free needle.

Like baseline, but each scan starts at a moving cursor (`needle`) instead of index 0 and wraps
around. After an allocation the needle moves to the next free index; a free below the needle pulls
it back, which keeps the first-free policy.
"""

from core import BaseAllocator, register


@register("needle", display_name="Sequential Needle")
class BitmapNeedleSequential(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)
        self.needle = 0

    def _find_next_free(self, start_idx):
        if self.size == 0:
            return -1
        idx = start_idx % self.size
        scanned = 0
        while scanned < self.size:
            if self.bitmap[idx] == 0:
                return idx
            idx = (idx + 1) % self.size
            scanned += 1
        return -1

    def allocate_blocks(self, count):
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        start = self.needle if self.needle < self.size else 0
        idx = start
        scanned = 0
        allocated = []

        while scanned < self.size and len(allocated) < count:
            if self.bitmap[idx] == 0:
                allocated.append(idx)
            idx = (idx + 1) % self.size
            scanned += 1

        if len(allocated) != count:
            return []

        for block_idx in allocated:
            self.bitmap[block_idx] = 1

        next_free = self._find_next_free(idx)
        self.needle = next_free if next_free != -1 else self.size
        return allocated

    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        freed_min = min(indices) if indices else None
        for idx in indices:
            self.bitmap[idx] = 0

        if freed_min is not None:
            if self.needle >= self.size or freed_min < self.needle:
                self.needle = freed_min
        return True

    def count_free_blocks(self):
        return self.free_blocks_remaining
