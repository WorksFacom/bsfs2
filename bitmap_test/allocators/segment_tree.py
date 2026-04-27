"""Segment Tree allocator for bitmap block management.

This implementation uses a sparse implicit segment tree that tracks
occupied blocks per range. Untouched nodes are considered fully free,
keeping memory proportional to touched regions instead of bitmap size.
"""

from core import BaseAllocator, register


@register("segtree", display_name="Segment Tree")
class BitmapSegmentTree(BaseAllocator):
    """Bitmap allocator using a sparse implicit segment tree."""

    def __init__(self, size):
        super().__init__(size)
        # node index -> occupied count in that interval
        self.occupied = {}

    def _node_occupied(self, node):
        return self.occupied.get(node, 0)

    def _set_block_occupied(self, index, is_occupied):
        if index < 0 or index >= self.size:
            raise IndexError(f"block index out of range: {index}")

        if self.size == 0:
            raise IndexError("cannot update block in empty bitmap")

        occupied = self.occupied
        path = []
        node = 1
        left = 0
        right = self.size - 1

        while left != right:
            path.append(node)
            mid = (left + right) // 2
            if index <= mid:
                node = node * 2
                right = mid
            else:
                node = node * 2 + 1
                left = mid + 1

        if is_occupied:
            occupied[node] = 1
        else:
            occupied.pop(node, None)

        for parent in reversed(path):
            left_child = parent * 2
            right_child = left_child + 1
            total = occupied.get(left_child, 0) + occupied.get(right_child, 0)
            if total:
                occupied[parent] = total
            else:
                occupied.pop(parent, None)

    def _allocate_first_free(self):
        if self.size == 0:
            return -1
        if self._node_occupied(1) >= self.size:
            return -1

        occupied = self.occupied
        node = 1
        left = 0
        right = self.size - 1

        while left != right:
            mid = (left + right) // 2
            left_node = node * 2
            left_len = mid - left + 1

            if occupied.get(left_node, 0) < left_len:
                node = left_node
                right = mid
            else:
                node = left_node + 1
                left = mid + 1

        self._set_block_occupied(left, True)
        return left

    def _rollback_allocated(self, allocated):
        self.rollback_bitmap_allocation(allocated)
        for idx in allocated:
            self._set_block_occupied(idx, False)

    def allocate_blocks(self, count):
        """Allocate count blocks."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []
        for _ in range(count):
            idx = self._allocate_first_free()
            if idx == -1:
                self._rollback_allocated(allocated)
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
            self._set_block_occupied(idx, False)
        return True

    def count_free_blocks(self):
        """Count total free blocks."""
        return self.free_blocks_remaining
