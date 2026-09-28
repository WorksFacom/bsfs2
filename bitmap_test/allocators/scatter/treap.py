"""
Lazy treap allocator.

Recycled indices live in a treap with random priorities, so insert/remove are expected O(log n).
Allocation takes the smaller of the treap minimum (leftmost node) and `next_fresh` (first
never-allocated block), removing the node or advancing the cursor. Frees reinsert into the treap
and never move the cursor.
"""

import random
from core import BaseAllocator, register


class TreapNode:
    def __init__(self, idx):
        self.idx = idx
        self.priority = random.random()
        self.left = None
        self.right = None


@register("treap", display_name="Treap")
class BitmapTreap(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)
        self.treap_root = None
        self.next_fresh = 0
    
    def _rotate_right(self, node):
        if node.left is None:
            return node
        
        new_root = node.left
        node.left = new_root.right
        new_root.right = node
        return new_root
    
    def _rotate_left(self, node):
        if node.right is None:
            return node
        
        new_root = node.right
        node.right = new_root.left
        new_root.left = node
        return new_root
    
    def _insert(self, node, new_node):
        """Insert, rotating up while the child outranks the parent (max-heap on priority)."""
        if node is None:
            return new_node
        
        if new_node.idx < node.idx:
            node.left = self._insert(node.left, new_node)
            if node.left.priority > node.priority:
                node = self._rotate_right(node)
        else:
            node.right = self._insert(node.right, new_node)
            if node.right.priority > node.priority:
                node = self._rotate_left(node)
        
        return node
    
    def _find_min_node(self, node):
        """Leftmost node, None if empty."""
        if node is None:
            return None
        while node.left is not None:
            node = node.left
        return node
    
    def _select_min_free(self):
        """Smallest free index across the treap and the fresh cursor, O(log n)."""
        treap_min_node = self._find_min_node(self.treap_root)
        treap_min = treap_min_node.idx if treap_min_node else None
        fresh_min = self.next_fresh if self.next_fresh < self.size else None
        
        if treap_min is None:
            return fresh_min
        if fresh_min is None:
            return treap_min
        return treap_min if treap_min < fresh_min else fresh_min
    
    def _delete(self, node, idx):
        if node is None:
            return None
        
        if idx < node.idx:
            node.left = self._delete(node.left, idx)
        elif idx > node.idx:
            node.right = self._delete(node.right, idx)
        else:
            if node.left is None:
                return node.right
            elif node.right is None:
                return node.left
            
            if node.left.priority > node.right.priority:
                node = self._rotate_right(node)
                node.right = self._delete(node.right, idx)
            else:
                node = self._rotate_left(node)
                node.left = self._delete(node.left, idx)
        
        return node
    
    def allocate_blocks(self, count):
        """O(count log n)."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []
        
        for _ in range(count):
            min_idx = self._select_min_free()
            if min_idx is None:
                return []
            
            self.bitmap[min_idx] = 1
            allocated.append(min_idx)
            
            if min_idx == self.next_fresh:
                self.next_fresh += 1
            else:
                self.treap_root = self._delete(self.treap_root, min_idx)
        
        return allocated
    
    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self.treap_root = self._insert(self.treap_root, TreapNode(idx))
        return True
    
    def count_free_blocks(self):
        return self.free_blocks_remaining
