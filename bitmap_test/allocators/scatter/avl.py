"""
Lazy AVL allocator.

Free blocks come from two places: `next_fresh`, the smallest never-allocated index, and an AVL
tree of recycled indices. Allocation takes the smaller of the two, advancing the cursor or
removing the tree node. Frees never move the cursor back, they go into the tree.
"""

from core import BaseAllocator, register


class AVLNode:
    def __init__(self, index):
        self.index = index
        self.left = None
        self.right = None
        self.height = 1


@register("avl", display_name="AVL Tree")
class BitmapAVL(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)
        self.root = None
        self.next_fresh = 0
    
    def find_free_block(self):
        """Smallest free index across the tree and the fresh cursor."""
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
        """O(count log n)."""
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
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self.root = self._insert(self.root, idx)
        return True
    
    def count_free_blocks(self):
        return self.free_blocks_remaining

    # ===== AVL tree =====
    
    def _insert(self, node, index):
        if not node:
            return AVLNode(index)
        
        if index < node.index:
            node.left = self._insert(node.left, index)
        else:
            node.right = self._insert(node.right, index)
        
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        balance = self._get_balance(node)
        
        # left-left
        if balance > 1 and index < node.left.index:
            return self._rotate_right(node)
        
        # right-right
        if balance < -1 and index > node.right.index:
            return self._rotate_left(node)
        
        # left-right
        if balance > 1 and index > node.left.index:
            node.left = self._rotate_left(node.left)
            return self._rotate_right(node)
        
        # right-left
        if balance < -1 and index < node.right.index:
            node.right = self._rotate_right(node.right)
            return self._rotate_left(node)
        
        return node
    
    def _delete(self, node, index):
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
        right_child = node.right
        node.right = right_child.left
        right_child.left = node
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        right_child.height = 1 + max(self._get_height(right_child.left), self._get_height(right_child.right))
        return right_child
    
    def _rotate_right(self, node):
        left_child = node.left
        node.left = left_child.right
        left_child.right = node
        node.height = 1 + max(self._get_height(node.left), self._get_height(node.right))
        left_child.height = 1 + max(self._get_height(left_child.left), self._get_height(left_child.right))
        return left_child
    
    def _get_height(self, node):
        return node.height if node else 0
    
    def _get_balance(self, node):
        return self._get_height(node.left) - self._get_height(node.right) if node else 0
