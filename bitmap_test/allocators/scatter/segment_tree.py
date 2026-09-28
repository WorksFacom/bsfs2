"""Sparse implicit segment tree allocator with a next_fresh cursor.

Never-allocated blocks come from `next_fresh` in O(1). Freed blocks go into a segment tree that
counts recycled blocks per interval, not occupied ones. The tree is sparse: only nodes with
recycled blocks under them exist. Allocation takes the smaller of the two sources, O(log N) when
it comes from the tree.

Deallocation walks down to the block's leaf, sets it to 1, and propagates sums up, pruning
ancestors that hit zero. Rolled-back blocks always go into the tree, whichever source they came
from, since next_fresh is never decremented.
"""

from core import BaseAllocator, register


@register("segtree", display_name="Segment Tree")
class BitmapSegmentTree(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)
        # node index -> recycled-free count in that interval
        self.recycled = {}
        # cursor: smallest never-allocated block index
        self.next_fresh = 0

    def _node_recycled(self, node):
        return self.recycled.get(node, 0)

    def _insert_recycled(self, index):
        if index < 0 or index >= self.size:
            raise IndexError(f"block index out of range: {index}")

        if self.size == 0:
            raise IndexError("cannot update block in empty bitmap")

        recycled = self.recycled
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

        recycled[node] = 1

        for parent in reversed(path):
            left_child = parent * 2
            right_child = left_child + 1
            total = recycled.get(left_child, 0) + recycled.get(right_child, 0)
            if total:
                recycled[parent] = total
            else:
                recycled.pop(parent, None)

    def _find_min_recycled_idx(self):
        """Minimum recycled index without removing it, O(log N)."""
        if self._node_recycled(1) == 0:
            return -1

        recycled = self.recycled
        node = 1
        left = 0
        right = self.size - 1

        while left != right:
            mid = (left + right) // 2
            left_node = node * 2

            if recycled.get(left_node, 0) > 0:
                node = left_node
                right = mid
            else:
                node = left_node + 1
                left = mid + 1

        return left

    def _pop_min_recycled(self):
        """Pop the minimum recycled index, O(log N)."""
        if self._node_recycled(1) == 0:
            return -1

        recycled = self.recycled
        path = []
        node = 1
        left = 0
        right = self.size - 1

        while left != right:
            path.append(node)
            mid = (left + right) // 2
            left_node = node * 2

            if recycled.get(left_node, 0) > 0:
                node = left_node
                right = mid
            else:
                node = left_node + 1
                left = mid + 1

        result = left
        recycled.pop(node, None)

        for parent in reversed(path):
            left_child = parent * 2
            right_child = left_child + 1
            total = recycled.get(left_child, 0) + recycled.get(right_child, 0)
            if total:
                recycled[parent] = total
            else:
                recycled.pop(parent, None)

        return result

    def _rollback_allocated(self, allocated):
        self.rollback_bitmap_allocation(allocated)
        for idx in allocated:
            self._insert_recycled(idx)

    def allocate_blocks(self, count):
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        recycled = self.recycled
        if not recycled:
            if self.next_fresh + count > self.size:
                return []
            start = self.next_fresh
            end = start + count
            self.bitmap[start:end] = b'\x01' * count
            self.next_fresh = end
            return list(range(start, end))

        allocated = []
        next_fresh = self.next_fresh
        size = self.size
        bitmap = self.bitmap

        for _ in range(count):
            recycled_has_blocks = bool(recycled)
            fresh_idx = next_fresh if next_fresh < size else -1

            if not recycled_has_blocks and fresh_idx == -1:
                self._rollback_allocated(allocated)
                return []

            if not recycled_has_blocks:
                idx = fresh_idx
                next_fresh += 1
            elif fresh_idx == -1:
                idx = self._pop_min_recycled()
            else:
                recycled_idx = self._find_min_recycled_idx()
                if fresh_idx <= recycled_idx:
                    idx = fresh_idx
                    next_fresh += 1
                else:
                    idx = self._pop_min_recycled()

            bitmap[idx] = 1
            allocated.append(idx)

        self.next_fresh = next_fresh
        return allocated

    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0

        for idx in sorted(indices):
            self._insert_recycled(idx)
        return True

    def count_free_blocks(self):
        return self.free_blocks_remaining
