"""
Allocator with a lazy model using AVL for recycled blocks.

Strategy idea:
- split free blocks into two sources: fresh range (`next_fresh`) and
  recycled blocks indexed in an AVL tree.
- always pick the smallest index between those two sources.

How `next_fresh` works:
- `next_fresh` points to the smallest index never allocated.
- when allocation uses this index, the cursor advances by +1.
- deallocated blocks do not move the cursor; they return through the AVL tree.

Allocation:
- combines two free-block sources: `next_fresh` (never-used blocks) and
  the AVL-tree minimum (previously deallocated blocks).
- always picks the smallest index between both sources.
- if it comes from `next_fresh`, advances the cursor; if from AVL, removes it
  from the tree.

Deallocation:
- validates indices, marks each block as free in the bitmap, and reinserts
  indices into the AVL tree for ordered reuse.
"""

from core import BaseAllocator, register


class AVLNode:
    """AVL Tree node"""
    
    def __init__(self, index):
        self.index = index
        self.left = None
        self.right = None
        self.height = 1


@register("avl", display_name="AVL Tree")
class BitmapAVL(BaseAllocator):
    """AVL Tree indexed Bitmap Allocator"""
    
    def __init__(self, size):
        """Initialize AVL tree for recycled blocks (lazy free model)."""
        super().__init__(size)
        self.root = None
        self.next_fresh = 0
    
    def find_free_block(self):
        """Find minimum free index among recycled AVL and fresh range."""
        tree_min = -1
        if self.root:
            node = self.root
            while node.left:
                node = node.left
            tree_min = node.index
        fresh_min = self.next_fresh if self.next_fresh < self.size else -1
        if tree_min == -1:
            return fresh_min
        if fresh_min == -1:
            return tree_min
        return min(tree_min, fresh_min)
    
    def allocate_blocks(self, count):
        """Allocate count free blocks using first-fit (O(count × log n))"""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            if idx == -1:
                return []
            self.bitmap[idx] = 1
            if idx == self.next_fresh:
                self.next_fresh += 1
            else:
                self.root = self._delete(self.root, idx)
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks by reinserting into AVL tree."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self.root = self._insert(self.root, idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks."""
        return self.free_blocks_remaining

    # ===== AVL Tree Operations =====
    
    def _insert(self, node, index):
        """Insert index into AVL tree"""
        if not node:
            return AVLNode(index)
        
        if index < node.index:
            node.left = self._insert(node.left, index)
        else:
            node.right = self._insert(node.right, index)
        
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        balance = self._get_balance(node)
        
        # Left-Left
        if balance > 1 and index < node.left.index:
            return self._rotate_right(node)
        
        # Right-Right
        if balance < -1 and index > node.right.index:
            return self._rotate_left(node)
        
        # Left-Right
        if balance > 1 and index > node.left.index:
            node.left = self._rotate_left(node.left)
            return self._rotate_right(node)
        
        # Right-Left
        if balance < -1 and index < node.right.index:
            node.right = self._rotate_right(node.right)
            return self._rotate_left(node)
        
        return node
    
    def _delete(self, node, index):
        """Delete index from AVL tree"""
        if not node:
            return None
        
        if index < node.index:
            node.left = self._delete(node.left, index)
        elif index > node.index:
            node.right = self._delete(node.right, index)
        else:
            if not node.left:
                return node.right
            elif not node.right:
                return node.left
            
            min_node = node.right
            while min_node.left:
                min_node = min_node.left
            node.index = min_node.index
            node.right = self._delete(node.right, min_node.index)
        
        if not node:
            return None
        
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
    
    def _rotate_left(self, node):
        """Left rotation"""
        right_child = node.right
        node.right = right_child.left
        right_child.left = node
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        right_child.height = 1 + max(self._get_height(right_child.left), self._get_height(right_child.right))
        return right_child
    
    def _rotate_right(self, node):
        """Right rotation"""
        left_child = node.left
        node.left = left_child.right
        left_child.right = node
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        left_child.height = 1 + max(self._get_height(left_child.left), self._get_height(left_child.right))
        return left_child
    
    def _get_height(self, node):
        """Get node height"""
        return node.height if node else 0
    
    def _get_balance(self, node):
        """Get balance factor"""
        return self._get_height(node.left) - self._get_height(node.right) if node else 0
