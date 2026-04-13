"""
AVL Tree implementation for bitmap block allocation
"""


class AVLNode:
    """AVL Tree node"""
    
    def __init__(self, index):
        self.index = index
        self.left = None
        self.right = None
        self.height = 1


class BitmapAVL:
    """AVL Tree indexed Bitmap Allocator"""
    
    def __init__(self, size):
        """Initialize bitmap of size blocks, all free (0)"""
        self.bitmap = [0] * size
        self.size = size
        self.root = None
        
        # Insert all indices as free in AVL tree
        for i in range(size):
            self.root = self._insert(self.root, i)
    
    def find_free_block(self):
        """Find minimum free index using AVL tree (O(log n))"""
        if not self.root:
            return -1
        
        node = self.root
        while node.left:
            node = node.left
        return node.index
    
    def allocate_blocks(self, count):
        """Allocate count free blocks using first-fit (O(count × log n))"""
        allocated = []
        
        try:
            for _ in range(count):
                idx = self.find_free_block()
                if idx == -1:
                    for allocated_idx in allocated:
                        self.root = self._insert(self.root, allocated_idx)
                        self.bitmap[allocated_idx] = 0
                    return []
                
                self.bitmap[idx] = 1
                self.root = self._delete(self.root, idx)
                allocated.append(idx)
            
            return sorted(allocated)
        except Exception:
            for allocated_idx in allocated:
                self.root = self._insert(self.root, allocated_idx)
                self.bitmap[allocated_idx] = 0
            return []
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks by reinserting into AVL tree"""
        try:
            for idx in indices:
                if self.bitmap[idx] == 0:
                    return False
                self.bitmap[idx] = 0
                self.root = self._insert(self.root, idx)
            return True
        except Exception:
            return False
    
    def count_free_blocks(self):
        """Count free blocks by tree traversal"""
        return self._count_recursive(self.root)
    
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
    
    def _count_recursive(self, node):
        """Recursively count nodes in tree"""
        if not node:
            return 0
        return 1 + self._count_recursive(node.left) + self._count_recursive(node.right)
