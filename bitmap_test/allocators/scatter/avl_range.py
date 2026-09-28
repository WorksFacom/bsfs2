"""
AVL tree of free ranges [start, end] keyed by `start`, scatter behavior.

Allocation drains the leftmost range one index at a time and drops the node once it's empty.
Deallocation groups freed indices into contiguous ranges and reinserts them, merging with
adjacent or overlapping neighbors.
"""

from core import BaseAllocator, register


class AVLRangeNode:
    """AVL node storing one free range [start, end]."""

    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.left = None
        self.right = None
        self.height = 1


@register("avl_range", display_name="AVL Range Tree")
class BitmapAVLRange(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)
        self.root = AVLRangeNode(0, size - 1) if size > 0 else None

    def allocate_blocks(self, count):
        """Take the count smallest free indices."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []
        for _ in range(count):
            idx = self._pop_min_free()
            if idx == -1:
                self.rollback_bitmap_allocation(allocated)
                for rollback_idx in allocated:
                    self._add_free_range(rollback_idx, rollback_idx)
                return []
            self.bitmap[idx] = 1
            allocated.append(idx)
        return allocated

    def deallocate_blocks(self, indices):
        """Reinsert freed indices as merged ranges."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0

        sorted_indices = sorted(indices)
        range_start = sorted_indices[0]
        range_end = range_start
        for idx in sorted_indices[1:]:
            if idx == range_end + 1:
                range_end = idx
                continue
            self._add_free_range(range_start, range_end)
            range_start = idx
            range_end = idx
        self._add_free_range(range_start, range_end)
        return True

    def count_free_blocks(self):
        return self.free_blocks_remaining

    def _pop_min_free(self):
        if not self.root:
            return -1

        node = self.root
        while node.left:
            node = node.left

        min_idx = node.start
        if node.start == node.end:
            self.root = self._delete(self.root, node.start)
        else:
            node.start += 1
        return min_idx

    def _add_free_range(self, start, end):
        if start > end:
            return

        while True:
            pred = self._find_predecessor(self.root, start)
            if pred and pred.end + 1 >= start:
                start = min(start, pred.start)
                end = max(end, pred.end)
                self.root = self._delete(self.root, pred.start)
                continue
            break

        while True:
            succ = self._find_successor(self.root, end)
            if succ and succ.start <= end + 1:
                end = max(end, succ.end)
                self.root = self._delete(self.root, succ.start)
                continue
            break

        self.root = self._insert(self.root, start, end)

    def _insert(self, node, start, end):
        if not node:
            return AVLRangeNode(start, end)

        if start < node.start:
            node.left = self._insert(node.left, start, end)
        else:
            node.right = self._insert(node.right, start, end)

        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        balance = self._get_balance(node)

        if balance > 1 and start < node.left.start:
            return self._rotate_right(node)
        if balance < -1 and start > node.right.start:
            return self._rotate_left(node)
        if balance > 1 and start > node.left.start:
            node.left = self._rotate_left(node.left)
            return self._rotate_right(node)
        if balance < -1 and start < node.right.start:
            node.right = self._rotate_right(node.right)
            return self._rotate_left(node)

        return node

    def _delete(self, node, start):
        if not node:
            return None

        if start < node.start:
            node.left = self._delete(node.left, start)
        elif start > node.start:
            node.right = self._delete(node.right, start)
        else:
            if not node.left:
                return node.right
            if not node.right:
                return node.left

            min_node = self._find_min(node.right)
            node.start = min_node.start
            node.end = min_node.end
            node.right = self._delete(node.right, min_node.start)

        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        balance = self._get_balance(node)

        if balance > 1 and self._get_balance(node.left) >= 0:
            return self._rotate_right(node)
        if balance > 1 and self._get_balance(node.left) < 0:
            node.left = self._rotate_left(node.left)
            return self._rotate_right(node)
        if balance < -1 and self._get_balance(node.right) <= 0:
            return self._rotate_left(node)
        if balance < -1 and self._get_balance(node.right) > 0:
            node.right = self._rotate_right(node.right)
            return self._rotate_left(node)

        return node

    def _find_min(self, node):
        while node.left:
            node = node.left
        return node

    def _find_predecessor(self, node, key):
        pred = None
        curr = node
        while curr:
            if curr.start < key:
                pred = curr
                curr = curr.right
            else:
                curr = curr.left
        return pred

    def _find_successor(self, node, key):
        succ = None
        curr = node
        while curr:
            if curr.start > key:
                succ = curr
                curr = curr.left
            else:
                curr = curr.right
        return succ

    def _rotate_left(self, node):
        right_child = node.right
        node.right = right_child.left
        right_child.left = node
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        right_child.height = 1 + max(
            self._get_height(right_child.left), self._get_height(right_child.right)
        )
        return right_child

    def _rotate_right(self, node):
        left_child = node.left
        node.left = left_child.right
        left_child.right = node
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        left_child.height = 1 + max(
            self._get_height(left_child.left), self._get_height(left_child.right)
        )
        return left_child

    def _get_height(self, node):
        return node.height if node else 0

    def _get_balance(self, node):
        return self._get_height(node.left) - self._get_height(node.right) if node else 0
