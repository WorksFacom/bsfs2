"""
Treap extent allocator with max_gap augmentation.

Free ranges live in a treap keyed by `start`; `max_gap` is the largest range in the subtree, so
best-fit and largest-range queries are expected O(log n). Policy lives in `RangeExtentAllocator`;
this module only implements the structure hooks. Treap rotations are local with no fixup pass,
so `max_gap` is an O(1) recompute after each rotate/insert/delete.
"""

import random

from core import register
from .extent_base import RangeExtentAllocator


class TreapRangeNode:
    """Treap node holding one free range [start, end]."""

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
    # inlined max over own size and child gaps, it runs on every node of every insert/delete path
    m = node.end - node.start + 1
    left = node.left
    if left is not None and left.max_gap > m:
        m = left.max_gap
    right = node.right
    if right is not None and right.max_gap > m:
        m = right.max_gap
    node.max_gap = m


@register("treap_range_extent", display_name="Treap Range Extent-First")
class BitmapTreapRangeExtent(RangeExtentAllocator):
    def _init_structure(self, size):
        self.treap_root = TreapRangeNode(0, size - 1) if size > 0 else None

    # ------------------------------------------------------------------
    # structure hooks
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
        self.treap_root, _ = self._delete(self.treap_root, node.start)
        return result

    def _insert_range(self, start, end):
        self.treap_root, _ = self._insert(self.treap_root, TreapRangeNode(start, end))

    def _delete_range(self, start):
        self.treap_root, _ = self._delete(self.treap_root, start)

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
    # treap internals
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
        # returns (subtree_root, changed). `changed` goes False once the new range stops affecting
        # this subtree's max_gap and no rotation bubbled through. treap rotations bubble up
        # contiguously from the leaf, so once they stop and the gap settles nothing above changes.
        if node is None:
            return new_node, True

        if new_node.start < node.start:
            node.left, changed = self._insert(node.left, new_node)
            if changed and node.left.priority > node.priority:
                node = self._rotate_right(node)
                return node, True
        else:
            node.right, changed = self._insert(node.right, new_node)
            if changed and node.right.priority > node.priority:
                node = self._rotate_left(node)
                return node, True

        if not changed:
            return node, False

        old = node.max_gap
        _update(node)
        return node, node.max_gap != old

    def _delete(self, node, start):
        # returns (subtree_root, changed), see `_insert`. delete rotations only push the target down,
        # so ancestors never rotate and can stop once max_gap settles.
        if node is None:
            return None, False

        if start < node.start:
            node.left, changed = self._delete(node.left, start)
            if not changed:
                return node, False
        elif start > node.start:
            node.right, changed = self._delete(node.right, start)
            if not changed:
                return node, False
        else:
            if node.left is None:
                return node.right, True
            if node.right is None:
                return node.left, True

            if node.left.priority > node.right.priority:
                node = self._rotate_right(node)
                node.right, _ = self._delete(node.right, start)
            else:
                node = self._rotate_left(node)
                node.left, _ = self._delete(node.left, start)
            _update(node)
            return node, True

        old = node.max_gap
        _update(node)
        return node, node.max_gap != old
