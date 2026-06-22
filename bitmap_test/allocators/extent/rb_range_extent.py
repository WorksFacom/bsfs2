"""
Red-Black tree allocator with extent-first allocation and max_gap augmentation.

Free space is a set of disjoint ranges held in a red-black tree keyed by `start`.
Each node stores `max_gap` = the largest range size in its subtree, giving
O(log n) best-fit and largest-range queries. This mirrors how real systems keep
an augmented red-black tree of free extents (e.g. Linux VMA management).

The allocation policy (best-fit extent, then lowest-address scatter fallback,
with coalescing on free) lives in `RangeExtentAllocator`; this module only
provides the red-black operations behind the structure hooks.

Augmentation maintenance follows the standard augmented-RB technique:
- each rotation recomputes the two pivot nodes (their subtree *sets* are the only
  ones that change shape);
- every raw structural change (leaf link on insert, splice on delete) propagates
  `max_gap` up to the root before any rebalancing fixup runs.
Re-coloring never affects `max_gap`, so the fixups need no augmentation logic.
"""

from enum import IntEnum

from core import register
from .extent_base import RangeExtentAllocator


class Color(IntEnum):
    RED = 0
    BLACK = 1


class RBRangeNode:
    """Red-Black tree node storing one free range [start, end] with max_gap."""

    __slots__ = ("start", "end", "color", "left", "right", "parent", "max_gap")

    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.color = Color.RED
        self.left = None
        self.right = None
        self.parent = None
        self.max_gap = end - start + 1

    @property
    def size(self):
        return self.end - self.start + 1


def _node_max_gap(node):
    return node.max_gap if node else 0


def _update(node):
    # Inlined max over (own size, child gaps): avoids two helper calls and the
    # max() builtin, which dominate dealloc-heavy workloads (millions of calls).
    m = node.end - node.start + 1
    left = node.left
    if left is not None and left.max_gap > m:
        m = left.max_gap
    right = node.right
    if right is not None and right.max_gap > m:
        m = right.max_gap
    node.max_gap = m


@register("rb_range_extent", display_name="Red-Black Range Extent-First")
class BitmapRBRangeExtent(RangeExtentAllocator):
    """Bitmap allocator using a red-black tree of free ranges (extent-first)."""

    def _init_structure(self, size):
        self.root = None
        if size > 0:
            node = RBRangeNode(0, size - 1)
            node.color = Color.BLACK
            self.root = node

    # ------------------------------------------------------------------
    # Structure hooks
    # ------------------------------------------------------------------

    def _find_best_fit(self, count):
        node = self._best_fit_node(self.root, count)
        return (node.start, node.end) if node else None

    def _pop_min_range(self):
        node = self._find_min(self.root)
        if node is None:
            return None
        result = (node.start, node.end)
        self._delete_node(node.start)
        return result

    def _insert_range(self, start, end):
        self._insert_node(start, end)

    def _delete_range(self, start):
        self._delete_node(start)

    def _predecessor_range(self, key):
        pred = None
        curr = self.root
        while curr:
            if curr.start < key:
                pred = curr
                curr = curr.right
            else:
                curr = curr.left
        return (pred.start, pred.end) if pred else None

    def _successor_range(self, key):
        succ = None
        curr = self.root
        while curr:
            if curr.start > key:
                succ = curr
                curr = curr.left
            else:
                curr = curr.right
        return (succ.start, succ.end) if succ else None

    # ------------------------------------------------------------------
    # Search helpers
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

    def _find_node(self, node, start):
        while node:
            if start == node.start:
                return node
            node = node.left if start < node.start else node.right
        return None

    def _find_min(self, node):
        if not node:
            return None
        while node.left:
            node = node.left
        return node

    def _update_to_root(self, node, force_through=None):
        """Recompute max_gap from `node` up to the root.

        Stops early once a node's max_gap is unchanged: an ancestor's max_gap is
        the max over its subtree, so if a node's value didn't change neither can
        any ancestor's. Freed ranges are mostly size-1 (smaller than existing
        gaps), so propagation usually stops after one step instead of O(log n).

        `force_through` is the node whose *own* range changed during a two-child
        delete; we must recompute it before the early-stop can fire, since the
        change there is not reflected by the spliced child's propagation alone.
        """
        while node:
            old = node.max_gap
            _update(node)
            if node is force_through:
                force_through = None
            if force_through is None and node.max_gap == old:
                return
            node = node.parent

    # ------------------------------------------------------------------
    # Insertion
    # ------------------------------------------------------------------

    def _insert_node(self, start, end):
        new_node = RBRangeNode(start, end)

        if not self.root:
            new_node.color = Color.BLACK
            self.root = new_node
            return

        parent = None
        current = self.root
        while current:
            parent = current
            if start < current.start:
                current = current.left
            elif start > current.start:
                current = current.right
            else:
                return

        new_node.parent = parent
        if start < parent.start:
            parent.left = new_node
        else:
            parent.right = new_node

        # The new leaf's own max_gap is already correct; only ancestors can
        # change, so propagate starting from the parent.
        self._update_to_root(parent)
        self._fix_insert(new_node)

    def _delete_node(self, start):
        node = self._find_node(self.root, start)
        if not node:
            return

        value_changed = None
        if node.left and node.right:
            successor = self._find_min(node.right)
            node.start = successor.start
            node.end = successor.end
            # `node` keeps its position but gained the successor's range, so it
            # must be recomputed even if intermediate nodes are unchanged.
            value_changed = node
            node = successor

        child = node.left if node.left else node.right

        if child:
            child.parent = node.parent

        if not node.parent:
            self.root = child
        else:
            if node.parent.left == node:
                node.parent.left = child
            else:
                node.parent.right = child

        # Propagate the set change up before rebalancing. Start at the spliced
        # node's parent (the child subtree is internally unchanged), forcing the
        # walk through the value-changed node in the two-child case.
        self._update_to_root(node.parent, value_changed)

        if node.color == Color.BLACK:
            if child:
                self._fix_delete(child)
            elif node.parent:
                self._fix_delete_null(node.parent, node == node.parent.left)

    # ------------------------------------------------------------------
    # Rotations (augmentation-aware)
    # ------------------------------------------------------------------

    def _rotate_left(self, node):
        right_child = node.right
        node.right = right_child.left

        if right_child.left:
            right_child.left.parent = node

        right_child.parent = node.parent

        if not node.parent:
            self.root = right_child
        elif node.parent.left == node:
            node.parent.left = right_child
        else:
            node.parent.right = right_child

        right_child.left = node
        node.parent = right_child

        _update(node)
        _update(right_child)

    def _rotate_right(self, node):
        left_child = node.left
        node.left = left_child.right

        if left_child.right:
            left_child.right.parent = node

        left_child.parent = node.parent

        if not node.parent:
            self.root = left_child
        elif node.parent.left == node:
            node.parent.left = left_child
        else:
            node.parent.right = left_child

        left_child.right = node
        node.parent = left_child

        _update(node)
        _update(left_child)

    # ------------------------------------------------------------------
    # Red-Black fixups (unchanged: they only touch color + call rotations)
    # ------------------------------------------------------------------

    def _fix_insert(self, node):
        while node.parent and node.parent.color == Color.RED:
            if not node.parent.parent:
                break
            if node.parent == node.parent.parent.left:
                uncle = node.parent.parent.right

                if uncle and uncle.color == Color.RED:
                    node.parent.color = Color.BLACK
                    uncle.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    node = node.parent.parent
                else:
                    if node == node.parent.right:
                        node = node.parent
                        self._rotate_left(node)

                    node.parent.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    self._rotate_right(node.parent.parent)
            else:
                uncle = node.parent.parent.left

                if uncle and uncle.color == Color.RED:
                    node.parent.color = Color.BLACK
                    uncle.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    node = node.parent.parent
                else:
                    if node == node.parent.left:
                        node = node.parent
                        self._rotate_right(node)

                    node.parent.color = Color.BLACK
                    node.parent.parent.color = Color.RED
                    self._rotate_left(node.parent.parent)

        self.root.color = Color.BLACK

    def _fix_delete(self, node):
        while node != self.root and self._get_color(node) == Color.BLACK:
            if node == node.parent.left:
                sibling = node.parent.right

                if self._get_color(sibling) == Color.RED:
                    sibling.color = Color.BLACK
                    node.parent.color = Color.RED
                    self._rotate_left(node.parent)
                    sibling = node.parent.right

                left_color = self._get_color(sibling.left) if sibling else Color.BLACK
                right_color = self._get_color(sibling.right) if sibling else Color.BLACK

                if left_color == Color.BLACK and right_color == Color.BLACK:
                    if sibling:
                        sibling.color = Color.RED
                    node = node.parent
                else:
                    if right_color == Color.BLACK:
                        if sibling.left:
                            sibling.left.color = Color.BLACK
                        sibling.color = Color.RED
                        self._rotate_right(sibling)
                        sibling = node.parent.right

                    if sibling:
                        sibling.color = node.parent.color
                    node.parent.color = Color.BLACK
                    if sibling and sibling.right:
                        sibling.right.color = Color.BLACK
                    self._rotate_left(node.parent)
                    node = self.root
            else:
                sibling = node.parent.left

                if self._get_color(sibling) == Color.RED:
                    sibling.color = Color.BLACK
                    node.parent.color = Color.RED
                    self._rotate_right(node.parent)
                    sibling = node.parent.left

                left_color = self._get_color(sibling.left) if sibling else Color.BLACK
                right_color = self._get_color(sibling.right) if sibling else Color.BLACK

                if right_color == Color.BLACK and left_color == Color.BLACK:
                    if sibling:
                        sibling.color = Color.RED
                    node = node.parent
                else:
                    if left_color == Color.BLACK:
                        if sibling.right:
                            sibling.right.color = Color.BLACK
                        sibling.color = Color.RED
                        self._rotate_left(sibling)
                        sibling = node.parent.left

                    if sibling:
                        sibling.color = node.parent.color
                    node.parent.color = Color.BLACK
                    if sibling and sibling.left:
                        sibling.left.color = Color.BLACK
                    self._rotate_right(node.parent)
                    node = self.root

        node.color = Color.BLACK

    def _fix_delete_null(self, parent, is_left):
        node = None
        while parent and (node is None or node.color == Color.BLACK):
            if not node or node == parent.left:
                sibling = parent.right

                if sibling and sibling.color == Color.RED:
                    sibling.color = Color.BLACK
                    parent.color = Color.RED
                    self._rotate_left(parent)
                    sibling = parent.right

                if sibling:
                    left_color = self._get_color(sibling.left)
                    right_color = self._get_color(sibling.right)

                    if left_color == Color.BLACK and right_color == Color.BLACK:
                        sibling.color = Color.RED
                        node = parent
                        parent = node.parent
                    else:
                        if right_color == Color.BLACK:
                            if sibling.left:
                                sibling.left.color = Color.BLACK
                            sibling.color = Color.RED
                            self._rotate_right(sibling)
                            sibling = parent.right

                        sibling.color = parent.color
                        parent.color = Color.BLACK
                        if sibling and sibling.right:
                            sibling.right.color = Color.BLACK
                        self._rotate_left(parent)
                        break
                else:
                    node = parent
                    parent = node.parent
            else:
                sibling = parent.left

                if sibling and sibling.color == Color.RED:
                    sibling.color = Color.BLACK
                    parent.color = Color.RED
                    self._rotate_right(parent)
                    sibling = parent.left

                if sibling:
                    left_color = self._get_color(sibling.left)
                    right_color = self._get_color(sibling.right)

                    if right_color == Color.BLACK and left_color == Color.BLACK:
                        sibling.color = Color.RED
                        node = parent
                        parent = node.parent
                    else:
                        if left_color == Color.BLACK:
                            if sibling.right:
                                sibling.right.color = Color.BLACK
                            sibling.color = Color.RED
                            self._rotate_left(sibling)
                            sibling = parent.left

                        sibling.color = parent.color
                        parent.color = Color.BLACK
                        if sibling and sibling.left:
                            sibling.left.color = Color.BLACK
                        self._rotate_right(parent)
                        break
                else:
                    node = parent
                    parent = node.parent

        if node:
            node.color = Color.BLACK

    def _get_color(self, node):
        return node.color if node else Color.BLACK
