"""
Allocator with a lazy model using Treap for recycled blocks.

Strategy idea:
- keep recycled blocks in a probabilistically balanced BST (treap) to support
  insert/remove with expected logarithmic cost.
- combine with fresh range (`next_fresh`) and always pick smallest index.

How `next_fresh` works:
- points to the first never-allocated block.
- advances only when allocation consumes fresh range.
- freed blocks return to treap and do not change this cursor.

Allocation:
- selects the smallest free index between `next_fresh` and treap minimum
  (leftmost node).
- if consuming `next_fresh`, advances cursor; if consuming recycled index,
  removes index from treap while preserving BST/heap properties.
- marks selected blocks as occupied in the bitmap.

Deallocation:
- validates indices, clears bitmap bits, and reinserts each index into treap
  with random priority to maintain expected balancing.
"""

import random
from core import BaseAllocator, register


class TreapNode:
    """Node in a Treap."""
    
    def __init__(self, idx):
        self.idx = idx
        self.priority = random.random()
        self.left = None
        self.right = None


@register("treap", display_name="Treap")
class BitmapTreap(BaseAllocator):
    """Bitmap allocator using Treap (randomized BST)."""
    
    def __init__(self, size):
        super().__init__(size)
        self.treap_root = None
        self.next_fresh = 0
    
    def _rotate_right(self, node):
        """Right rotation for treap rebalancing."""
        if node.left is None:
            return node
        
        new_root = node.left
        node.left = new_root.right
        new_root.right = node
        return new_root
    
    def _rotate_left(self, node):
        """Left rotation for treap rebalancing."""
        if node.right is None:
            return node
        
        new_root = node.right
        node.right = new_root.left
        new_root.left = node
        return new_root
    
    def _insert(self, node, new_node):
        """Insert into treap maintaining heap property via rotations."""
        if node is None:
            return new_node
        
        if new_node.idx < node.idx:
            node.left = self._insert(node.left, new_node)
            # Rotate right if left child has higher priority (max-heap)
            if node.left.priority > node.priority:
                node = self._rotate_right(node)
        else:
            node.right = self._insert(node.right, new_node)
            # Rotate left if right child has higher priority (max-heap)
            if node.right.priority > node.priority:
                node = self._rotate_left(node)
        
        return node
    
    def _find_min_node(self, node):
        """Find node with minimum index (leftmost). Returns None if empty."""
        if node is None:
            return None
        while node.left is not None:
            node = node.left
        return node
    
    def _select_min_free(self):
        """Select minimum free index from treap or fresh cursor - O(log n)."""
        treap_min_node = self._find_min_node(self.treap_root)
        treap_min = treap_min_node.idx if treap_min_node else None
        fresh_min = self.next_fresh if self.next_fresh < self.size else None
        
        if treap_min is None:
            return fresh_min
        if fresh_min is None:
            return treap_min
        return treap_min if treap_min < fresh_min else fresh_min
    
    def _delete(self, node, idx):
        """Delete node with given index from treap."""
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
        """Allocate count blocks using min-finding from treap - O(count × log n)."""
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
        """Deallocate blocks."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            # Reinsert into treap
            self.treap_root = self._insert(self.treap_root, TreapNode(idx))
        return True
    
    def count_free_blocks(self):
        """Count total free blocks."""
        return self.free_blocks_remaining
