"""
B+tree allocator with extent-first allocation and max_size augmentation.

Free space is a set of disjoint ranges held in a B+tree keyed by `start`. Data
(the `(start, end)` ranges) lives only in the leaves, which are doubly linked for
O(1) in-order traversal; internal nodes hold separator keys only. Each node is
augmented with `max_size` = the largest range size in its subtree, enabling
contiguous-fit search. This mirrors the general-purpose-filesystem design (e.g.
XFS keeps free extents in B+trees) — shallow high-fanout trees with linked leaves
give good cache behavior and locality-friendly scans.

The allocation policy (best-fit extent, then lowest-address scatter fallback,
with coalescing on free) lives in `RangeExtentAllocator`; this module only
provides the B+tree operations behind the structure hooks.

`max_size` is maintained by recomputing affected nodes on each split / borrow /
merge and propagating up to the root after every insert and delete.
"""

import bisect

from core import register
from .extent_base import RangeExtentAllocator


class BPRangeNode:
    """B+tree node. Leaves store parallel keys/ends; internals store keys/children."""

    __slots__ = ("is_leaf", "keys", "ends", "children", "parent", "next", "prev", "max_size")

    def __init__(self, is_leaf=True):
        self.is_leaf = is_leaf
        self.keys = []          # starts (leaf) or separators (internal)
        self.ends = []          # parallel ends (leaf only)
        self.children = []      # internal only
        self.parent = None
        self.next = None        # leaf links
        self.prev = None
        self.max_size = 0


@register("bplustree_extent", display_name="B+Tree Extent-First")
class BitmapBPlusTreeExtent(RangeExtentAllocator):
    """Bitmap allocator using a B+tree of free ranges (extent-first)."""

    ORDER = 64  # max keys/entries per node (even); min fill = ORDER // 2

    def _init_structure(self, size):
        self.max_keys = self.ORDER
        self.min_keys = self.ORDER // 2
        root = BPRangeNode(is_leaf=True)
        if size > 0:
            root.keys.append(0)
            root.ends.append(size - 1)
            root.max_size = size
        self.root = root

    # ------------------------------------------------------------------
    # Augmentation helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _recompute_node(node):
        # Manual loops instead of max(generator): this is the hottest call on
        # dealloc-heavy workloads, and the generator + max() builtin overhead
        # dominated the profile.
        if node.is_leaf:
            keys = node.keys
            ends = node.ends
            m = 0
            for j in range(len(keys)):
                sz = ends[j] - keys[j] + 1
                if sz > m:
                    m = sz
            node.max_size = m
        else:
            m = 0
            for c in node.children:
                if c.max_size > m:
                    m = c.max_size
            node.max_size = m

    def _recompute_path(self, node):
        """Propagate up, stopping once a node's max_size is unchanged.

        Correct only when `node` has *not* been pre-recomputed (otherwise the
        change is masked); callers on the structural-rebalance path that
        pre-recompute their start node use `_recompute_path_full` instead.
        """
        while node:
            old = node.max_size
            self._recompute_node(node)
            if node.max_size == old:
                return
            node = node.parent

    def _recompute_path_full(self, node):
        """Unconditional propagation to the root (used after rebalancing)."""
        while node:
            self._recompute_node(node)
            node = node.parent

    @staticmethod
    def _child_index(parent, node):
        for i, c in enumerate(parent.children):
            if c is node:
                return i
        raise ValueError("child not found in parent")

    # ------------------------------------------------------------------
    # Navigation
    # ------------------------------------------------------------------

    def _find_leaf(self, start):
        node = self.root
        while not node.is_leaf:
            i = bisect.bisect_right(node.keys, start)
            node = node.children[i]
        return node

    def _leftmost_leaf(self):
        node = self.root
        while not node.is_leaf:
            node = node.children[0]
        return node

    # ------------------------------------------------------------------
    # Structure hooks
    # ------------------------------------------------------------------

    def _find_best_fit(self, count):
        res = self._best_fit(self.root, count)
        return (res[1], res[2]) if res else None

    def _best_fit(self, node, count):
        if node is None or node.max_size < count:
            return None

        best = None
        if node.is_leaf:
            for s, e in zip(node.keys, node.ends):
                sz = e - s + 1
                if sz >= count and (best is None or sz < best[0]):
                    best = (sz, s, e)
            return best

        for child in node.children:
            if child.max_size >= count:
                cand = self._best_fit(child, count)
                if cand and (best is None or cand[0] < best[0]):
                    best = cand
        return best

    def _pop_min_range(self):
        leaf = self._leftmost_leaf()
        if not leaf.keys:
            return None
        start, end = leaf.keys[0], leaf.ends[0]
        self._delete_range(start)
        return (start, end)

    def _predecessor_range(self, key):
        leaf = self._find_leaf(key)
        i = bisect.bisect_left(leaf.keys, key) - 1
        if i >= 0:
            return (leaf.keys[i], leaf.ends[i])
        node = leaf.prev
        while node is not None and not node.keys:
            node = node.prev
        if node and node.keys:
            return (node.keys[-1], node.ends[-1])
        return None

    def _successor_range(self, key):
        leaf = self._find_leaf(key)
        i = bisect.bisect_right(leaf.keys, key)
        if i < len(leaf.keys):
            return (leaf.keys[i], leaf.ends[i])
        node = leaf.next
        while node is not None and not node.keys:
            node = node.next
        if node and node.keys:
            return (node.keys[0], node.ends[0])
        return None

    # ------------------------------------------------------------------
    # Insertion
    # ------------------------------------------------------------------

    def _insert_range(self, start, end):
        leaf = self._find_leaf(start)
        i = bisect.bisect_left(leaf.keys, start)
        leaf.keys.insert(i, start)
        leaf.ends.insert(i, end)

        # max_size can only grow on insert, so propagate the new size up,
        # stopping at the first ancestor that already covers it (size-1 frees
        # stop at the leaf). A split preserves the subtree's range set, so it
        # only needs the local recomputes done in `_split_if_needed`.
        sz = end - start + 1
        node = leaf
        while node is not None and sz > node.max_size:
            node.max_size = sz
            node = node.parent

        if len(leaf.keys) > self.max_keys:
            self._split_if_needed(leaf)

    def _split_if_needed(self, node):
        if len(node.keys) <= self.max_keys:
            return

        mid = len(node.keys) // 2
        new = BPRangeNode(is_leaf=node.is_leaf)

        if node.is_leaf:
            sep = node.keys[mid]
            new.keys = node.keys[mid:]
            new.ends = node.ends[mid:]
            node.keys = node.keys[:mid]
            node.ends = node.ends[:mid]
            new.next = node.next
            new.prev = node
            if node.next:
                node.next.prev = new
            node.next = new
        else:
            sep = node.keys[mid]
            new.keys = node.keys[mid + 1:]
            new.children = node.children[mid + 1:]
            node.keys = node.keys[:mid]
            node.children = node.children[:mid + 1]
            for c in new.children:
                c.parent = new

        self._recompute_node(node)
        self._recompute_node(new)

        parent = node.parent
        if parent is None:
            parent = BPRangeNode(is_leaf=False)
            parent.keys = [sep]
            parent.children = [node, new]
            node.parent = parent
            new.parent = parent
            self.root = parent
            self._recompute_node(parent)
            return

        idx = bisect.bisect_left(parent.keys, sep)
        parent.keys.insert(idx, sep)
        parent.children.insert(idx + 1, new)
        new.parent = parent
        self._recompute_node(parent)
        self._split_if_needed(parent)

    # ------------------------------------------------------------------
    # Deletion
    # ------------------------------------------------------------------

    def _delete_range(self, start):
        leaf = self._find_leaf(start)
        i = bisect.bisect_left(leaf.keys, start)
        if i >= len(leaf.keys) or leaf.keys[i] != start:
            return
        removed_sz = leaf.ends[i] - leaf.keys[i] + 1
        leaf.keys.pop(i)
        leaf.ends.pop(i)

        if len(leaf.keys) >= self.min_keys or leaf is self.root:
            # No structural change. max_size can only have shrunk, and only if
            # we removed a maximal entry; otherwise nothing changes anywhere.
            if removed_sz >= leaf.max_size:
                self._recompute_node(leaf)
                self._recompute_path(leaf.parent)
            return

        # Underflow: borrow/merge, then propagate unconditionally to the root.
        self._recompute_node(leaf)
        self._rebalance_after_delete(leaf)

    def _rebalance_after_delete(self, node):
        if node is self.root:
            if not node.is_leaf and len(node.children) == 1:
                self.root = node.children[0]
                self.root.parent = None
            else:
                self._recompute_node(node)
            return

        if len(node.keys) >= self.min_keys:
            self._recompute_path_full(node)
            return

        parent = node.parent
        idx = self._child_index(parent, node)
        left = parent.children[idx - 1] if idx > 0 else None
        right = parent.children[idx + 1] if idx + 1 < len(parent.children) else None

        if left and len(left.keys) > self.min_keys:
            self._borrow_from_left(parent, idx)
            self._recompute_path_full(parent)
            return
        if right and len(right.keys) > self.min_keys:
            self._borrow_from_right(parent, idx)
            self._recompute_path_full(parent)
            return

        if left:
            self._merge(parent, idx - 1)
        else:
            self._merge(parent, idx)
        self._recompute_node(parent)
        self._rebalance_after_delete(parent)

    def _borrow_from_left(self, parent, idx):
        node = parent.children[idx]
        left = parent.children[idx - 1]
        if node.is_leaf:
            node.keys.insert(0, left.keys.pop())
            node.ends.insert(0, left.ends.pop())
            parent.keys[idx - 1] = node.keys[0]
        else:
            node.keys.insert(0, parent.keys[idx - 1])
            moved = left.children.pop()
            moved.parent = node
            node.children.insert(0, moved)
            parent.keys[idx - 1] = left.keys.pop()
        self._recompute_node(left)
        self._recompute_node(node)
        self._recompute_node(parent)

    def _borrow_from_right(self, parent, idx):
        node = parent.children[idx]
        right = parent.children[idx + 1]
        if node.is_leaf:
            node.keys.append(right.keys.pop(0))
            node.ends.append(right.ends.pop(0))
            parent.keys[idx] = right.keys[0]
        else:
            node.keys.append(parent.keys[idx])
            moved = right.children.pop(0)
            moved.parent = node
            node.children.append(moved)
            parent.keys[idx] = right.keys.pop(0)
        self._recompute_node(right)
        self._recompute_node(node)
        self._recompute_node(parent)

    def _merge(self, parent, j):
        """Merge children[j] (left) and children[j+1] (right) into left."""
        left = parent.children[j]
        right = parent.children[j + 1]

        if left.is_leaf:
            left.keys.extend(right.keys)
            left.ends.extend(right.ends)
            left.next = right.next
            if right.next:
                right.next.prev = left
        else:
            left.keys.append(parent.keys[j])
            left.keys.extend(right.keys)
            for c in right.children:
                c.parent = left
            left.children.extend(right.children)

        parent.keys.pop(j)
        parent.children.pop(j + 1)
        self._recompute_node(left)
