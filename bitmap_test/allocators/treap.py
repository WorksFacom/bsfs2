"""
Treap (Tree + Heap) allocator for bitmap block management.

A randomized self-balancing BST that combines:
- BST property for ordering (fast search)
- Heap property for balancing (simpler than RB-Tree rotations)

Uses random priorities to maintain expected O(log n) height.
Simpler to implement than Red-Black Trees while maintaining same complexity.
"""

import random


class TreapNode:
    """Node in a Treap."""
    
    def __init__(self, idx):
        self.idx = idx
        self.priority = random.random()  # Random priority for heap property
        self.left = None
        self.right = None
    
    def __lt__(self, other):
        """Compare by index (BST property)."""
        return self.idx < other.idx


class BitmapTreap:
    """Bitmap allocator using Treap (randomized BST)."""
    
    def __init__(self, size):
        self.size = size
        self.bitmap = [0] * size  # 0 = free, 1 = occupied
        self.treap_root = None
        
        # Initialize treap with all free blocks
        for i in range(size):
            self.treap_root = self._insert(self.treap_root, TreapNode(i))
    
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
        """Find node with minimum index (leftmost)."""
        if node is None:
            return None
        while node.left is not None:
            node = node.left
        return node
    
    def _delete(self, node, idx):
        """Delete node with given index from treap."""
        if node is None:
            return None
        
        if idx < node.idx:
            node.left = self._delete(node.left, idx)
        elif idx > node.idx:
            node.right = self._delete(node.right, idx)
        else:
            # Found the node to delete
            if node.left is None:
                return node.right
            elif node.right is None:
                return node.left
            
            # Both children exist - rotate to push node down
            if node.left.priority > node.right.priority:
                node = self._rotate_right(node)
                node.right = self._delete(node.right, idx)
            else:
                node = self._rotate_left(node)
                node.left = self._delete(node.left, idx)
        
        return node
    
    def _find_min(self, node):
        """Find minimum index in treap."""
        if node is None:
            return -1
        return self._find_min_node(node).idx
    
    def allocate_blocks(self, count):
        """Allocate count blocks using min-finding from treap."""
        if count <= 0:
            return []

        allocated = []
        
        for _ in range(count):
            # Find minimum free block
            min_idx = self._find_min(self.treap_root)
            
            if min_idx == -1:
                for allocated_idx in allocated:
                    self.bitmap[allocated_idx] = 0
                    self.treap_root = self._insert(self.treap_root, TreapNode(allocated_idx))
                return []
            
            # Mark as occupied
            self.bitmap[min_idx] = 1
            allocated.append(min_idx)
            
            # Remove from treap
            self.treap_root = self._delete(self.treap_root, min_idx)
        
        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks."""
        if len(set(indices)) != len(indices):
            return False

        for idx in indices:
            if idx < 0 or idx >= self.size:
                return False
            if self.bitmap[idx] == 0:
                return False

        for idx in indices:
            self.bitmap[idx] = 0
            # Reinsert into treap
            self.treap_root = self._insert(self.treap_root, TreapNode(idx))
        return True
    
    def count_free_blocks(self):
        """Count total free blocks in treap."""
        def count_nodes(node):
            if node is None:
                return 0
            return 1 + count_nodes(node.left) + count_nodes(node.right)
        
        return count_nodes(self.treap_root)
