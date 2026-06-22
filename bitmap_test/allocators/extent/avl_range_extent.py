"""
AVL allocator with extent-first allocation and max_gap augmentation.

Strategy idea:
- represent free space as disjoint ranges in an AVL tree keyed by `start`.
- each node stores `max_gap` = max(size of any range in this subtree),
  enabling O(log n) best-fit and largest-range queries.
- initialization starts with one range [0, size-1] when bitmap is non-empty.

Allocation (extent-first):
- first, searches for a single contiguous range that fits N (best-fit).
- if no single range fits, falls back to taking from the largest range(s)
  until N blocks are allocated (non-contiguous).
- all-or-nothing: partial allocations are rolled back on failure.

Deallocation:
- validates indices, marks them free in bitmap, groups contiguous indices
  into ranges, and reinserts each range with neighbor merging.
"""

from core import BaseAllocator, register


class AVLRangeExtentNode:
    """AVL node storing one free range [start, end] with max_gap augmentation."""

    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.left = None
        self.right = None
        self.height = 1
        self.max_gap = end - start + 1

    @property
    def size(self):
        return self.end - self.start + 1


def _node_max_gap(node):
    return node.max_gap if node else 0


def _node_height(node):
    return node.height if node else 0


def _update_node(node):
    if not node:
        return
    # Inlined height + max_gap recompute: avoids the helper calls and two max()
    # builtins on the hot insert/delete path.
    left = node.left
    right = node.right
    lh = left.height if left is not None else 0
    rh = right.height if right is not None else 0
    node.height = lh + 1 if lh > rh else rh + 1
    m = node.end - node.start + 1
    if left is not None and left.max_gap > m:
        m = left.max_gap
    if right is not None and right.max_gap > m:
        m = right.max_gap
    node.max_gap = m


@register("avl_range_extent", display_name="AVL Range Extent-First")
class BitmapAVLRangeExtent(BaseAllocator):
    """Bitmap allocator using AVL tree of free ranges with extent-first allocation."""

    # Best-fit but with a *largest-range* fallback (unlike the lowest-address
    # fallback shared by the treap/rb/bplustree extent allocators), so it forms
    # its own bitmap-equivalence group.
    CONSISTENCY_GROUP = "extent_bestfit_largest"

    def __init__(self, size):
        super().__init__(size)
        self.root = AVLRangeExtentNode(0, size - 1) if size > 0 else None

    def allocate_blocks(self, count):
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        # Step 1: try a single contiguous range (best-fit)
        node = self._find_best_fit(self.root, count)
        if node:
            range_start, range_end = node.start, node.end
            self.root, _ = self._delete(self.root, range_start)
            allocated = list(range(range_start, range_start + count))
            if range_start + count <= range_end:
                self.root, _ = self._insert(self.root, range_start + count, range_end)
            for idx in allocated:
                self.bitmap[idx] = 1
            return allocated

        # Step 2: fallback — take from largest ranges until N satisfied
        remaining = count
        while remaining > 0:
            node = self._find_largest(self.root)
            if not node:
                self.rollback_bitmap_allocation(allocated)
                for rollback_idx in allocated:
                    self._add_free_range(rollback_idx, rollback_idx)
                return []

            range_start, range_end = node.start, node.end
            self.root, _ = self._delete(self.root, range_start)
            take = min(remaining, range_end - range_start + 1)
            new_blocks = list(range(range_start, range_start + take))
            allocated.extend(new_blocks)
            if range_start + take <= range_end:
                self.root, _ = self._insert(self.root, range_start + take, range_end)
            remaining -= take

        for idx in allocated:
            self.bitmap[idx] = 1
        return allocated

    def deallocate_blocks(self, indices):
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

    # ------------------------------------------------------------------
    # Range tree operations
    # ------------------------------------------------------------------

    def _add_free_range(self, start, end):
        if start > end:
            return

        while True:
            pred = self._find_predecessor(self.root, start)
            if pred and pred.end + 1 >= start:
                start = min(start, pred.start)
                end = max(end, pred.end)
                self.root, _ = self._delete(self.root, pred.start)
                continue
            break

        while True:
            succ = self._find_successor(self.root, end)
            if succ and succ.start <= end + 1:
                end = max(end, succ.end)
                self.root, _ = self._delete(self.root, succ.start)
                continue
            break

        self.root, _ = self._insert(self.root, start, end)

    # ------------------------------------------------------------------
    # AVL insertion / deletion
    # ------------------------------------------------------------------

    def _insert(self, node, start, end):
        # Returns (subtree_root, changed): `changed` is False once neither the
        # height nor the max_gap of this subtree changed, so ancestors can stop
        # recomputing/rebalancing. An AVL insert rotation restores the subtree's
        # pre-insert height, so height changes never propagate past a rotation.
        if not node:
            return AVLRangeExtentNode(start, end), True

        if start < node.start:
            node.left, changed = self._insert(node.left, start, end)
        else:
            node.right, changed = self._insert(node.right, start, end)

        if not changed:
            return node, False

        old_height = node.height
        old_gap = node.max_gap
        _update_node(node)
        balance = self._get_balance(node)

        if balance > 1 and start < node.left.start:
            node = self._rotate_right(node)
        elif balance < -1 and start > node.right.start:
            node = self._rotate_left(node)
        elif balance > 1 and start > node.left.start:
            node.left = self._rotate_left(node.left)
            node = self._rotate_right(node)
        elif balance < -1 and start < node.right.start:
            node.right = self._rotate_right(node.right)
            node = self._rotate_left(node)

        return node, (node.height != old_height or node.max_gap != old_gap)

    def _delete(self, node, start):
        # Returns (subtree_root, changed); see `_insert`. A delete can shrink the
        # subtree height and cascade rotations upward, so we keep unwinding while
        # height or max_gap changes and stop as soon as both are stable.
        if not node:
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
            if not node.left:
                return node.right, True
            if not node.right:
                return node.left, True

            min_node = self._find_min(node.right)
            node.start = min_node.start
            node.end = min_node.end
            # node's own range changed, so it must always be recomputed below.
            node.right, _ = self._delete(node.right, min_node.start)

        old_height = node.height
        old_gap = node.max_gap
        _update_node(node)
        balance = self._get_balance(node)

        if balance > 1 and self._get_balance(node.left) >= 0:
            node = self._rotate_right(node)
        elif balance > 1 and self._get_balance(node.left) < 0:
            node.left = self._rotate_left(node.left)
            node = self._rotate_right(node)
        elif balance < -1 and self._get_balance(node.right) <= 0:
            node = self._rotate_left(node)
        elif balance < -1 and self._get_balance(node.right) > 0:
            node.right = self._rotate_right(node.right)
            node = self._rotate_left(node)

        return node, (node.height != old_height or node.max_gap != old_gap)

    # ------------------------------------------------------------------
    # Search helpers
    # ------------------------------------------------------------------

    def _find_best_fit(self, node, count):
        if not node or node.max_gap < count:
            return None

        best = None
        if node.left and node.left.max_gap >= count:
            best = self._find_best_fit(node.left, count)

        if node.size >= count and (not best or node.size < best.size):
            best = node

        if node.right and node.right.max_gap >= count:
            right_best = self._find_best_fit(node.right, count)
            if right_best and (not best or right_best.size < best.size):
                best = right_best

        return best

    def _find_largest(self, node):
        if not node:
            return None
        if node.left and node.left.max_gap == node.max_gap:
            return self._find_largest(node.left)
        if node.right and node.right.max_gap == node.max_gap:
            return self._find_largest(node.right)
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

    # ------------------------------------------------------------------
    # Rotations
    # ------------------------------------------------------------------

    def _rotate_left(self, node):
        right_child = node.right
        node.right = right_child.left
        right_child.left = node
        _update_node(node)
        _update_node(right_child)
        return right_child

    def _rotate_right(self, node):
        left_child = node.left
        node.left = left_child.right
        left_child.right = node
        _update_node(node)
        _update_node(left_child)
        return left_child

    def _get_height(self, node):
        return _node_height(node)

    def _get_balance(self, node):
        return _node_height(node.left) - _node_height(node.right) if node else 0

