"""
Treap allocator with extent-first allocation and max_gap augmentation.

Free space is a set of disjoint ranges held in a treap (randomized BST) keyed by
`start`. Each node stores `max_gap` = the largest range size in its subtree,
which makes best-fit and largest-range queries O(log n) expected.

The allocation policy (best-fit extent, then lowest-address scatter fallback,
with coalescing on free) lives in `RangeExtentAllocator`; this module only
provides the treap operations behind the structure hooks. Treap rotations are
local (no rebalancing fixup), so maintaining `max_gap` is just an O(1) recompute
after each rotate / insert / delete.
"""

import random

from core import register
from .extent_base import RangeExtentAllocator


class TreapRangeNode:
    """Treap node storing one free range [start, end] with max_gap augmentation."""

    __slots__ = ("start", "end", "priority", "left", "right", "max_gap")

    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.priority = random.random()
        self.left = None
        self.right = None
        self.max_gap = end - start + 1

    @property
    def size(self):
        return self.end - self.start + 1


def _node_max_gap(node):
    return node.max_gap if node else 0


def _update(node):
    node.max_gap = max(
        node.end - node.start + 1,
        _node_max_gap(node.left),
        _node_max_gap(node.right),
    )


@register("treap_range_extent", display_name="Treap Range Extent-First")
class BitmapTreapRangeExtent(RangeExtentAllocator):
    """Bitmap allocator using a treap of free ranges with extent-first allocation."""

    def _init_structure(self, size):
        self.treap_root = TreapRangeNode(0, size - 1) if size > 0 else None

    # ------------------------------------------------------------------
    # Structure hooks
    # ------------------------------------------------------------------

    def _find_best_fit(self, count):
        node = self._best_fit_node(self.treap_root, count)
        return (node.start, node.end) if node else None

    def _pop_min_range(self):
        node = self.treap_root
        if node is None:
            return None
        while node.left:
            node = node.left
        result = (node.start, node.end)
        self.treap_root = self._delete(self.treap_root, node.start)
        return result

    def _insert_range(self, start, end):
        self.treap_root = self._insert(self.treap_root, TreapRangeNode(start, end))

    def _delete_range(self, start):
        self.treap_root = self._delete(self.treap_root, start)

    def _predecessor_range(self, key):
        pred = None
        curr = self.treap_root
        while curr:
            if curr.start < key:
                pred = curr
                curr = curr.right
            else:
                curr = curr.left
        return (pred.start, pred.end) if pred else None

    def _successor_range(self, key):
        succ = None
        curr = self.treap_root
        while curr:
            if curr.start > key:
                succ = curr
                curr = curr.left
            else:
                curr = curr.right
        return (succ.start, succ.end) if succ else None

    # ------------------------------------------------------------------
    # Treap internals
    # ------------------------------------------------------------------

    def _best_fit_node(self, node, count):
        if not node or node.max_gap < count:
            return None

        best = None
        if node.left and node.left.max_gap >= count:
            best = self._best_fit_node(node.left, count)

        if node.size >= count and (not best or node.size < best.size):
            best = node

        if node.right and node.right.max_gap >= count:
            right_best = self._best_fit_node(node.right, count)
            if right_best and (not best or right_best.size < best.size):
                best = right_best

        return best

    def _rotate_right(self, node):
        new_root = node.left
        node.left = new_root.right
        new_root.right = node
        _update(node)
        _update(new_root)
        return new_root

    def _rotate_left(self, node):
        new_root = node.right
        node.right = new_root.left
        new_root.left = node
        _update(node)
        _update(new_root)
        return new_root

    def _insert(self, node, new_node):
        if node is None:
            return new_node

        if new_node.start < node.start:
            node.left = self._insert(node.left, new_node)
            if node.left.priority > node.priority:
                node = self._rotate_right(node)
        else:
            node.right = self._insert(node.right, new_node)
            if node.right.priority > node.priority:
                node = self._rotate_left(node)

        _update(node)
        return node

    def _delete(self, node, start):
        if node is None:
            return None

        if start < node.start:
            node.left = self._delete(node.left, start)
        elif start > node.start:
            node.right = self._delete(node.right, start)
        else:
            if node.left is None:
                return node.right
            if node.right is None:
                return node.left

            if node.left.priority > node.right.priority:
                node = self._rotate_right(node)
                node.right = self._delete(node.right, start)
            else:
                node = self._rotate_left(node)
                node.left = self._delete(node.left, start)

        _update(node)
        return node
