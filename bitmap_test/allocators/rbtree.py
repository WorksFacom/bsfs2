"""
Red-Black Tree implementation for bitmap block allocation
"""

from enum import IntEnum

from core import BaseAllocator, register


class Color(IntEnum):
    """Red-Black Tree node color"""
    RED = 0
    BLACK = 1


class RBNode:
    """Red-Black Tree node"""
    
    def __init__(self, index):
        self.index = index
        self.color = Color.RED
        self.left = None
        self.right = None
        self.parent = None


@register("rb", display_name="Red-Black Tree")
class BitmapRB(BaseAllocator):
    """Red-Black Tree indexed Bitmap Allocator"""
    
    def __init__(self, size):
        """Initialize bitmap of size blocks, all free (0)"""
        super().__init__(size)
        self.root = None
        self.free_blocks = set(range(size))
    
    def find_free_block(self):
        """Find minimum free index - O(log n)"""
        if not self.free_blocks:
            return -1
        return min(self.free_blocks)
    
    def allocate_blocks(self, count):
        """Allocate count free blocks - O(count × log n)"""
        if count <= 0:
            return []
        if len(self.free_blocks) < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            self.free_blocks.remove(idx)
            self.bitmap[idx] = 1
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks - O(count × log n)"""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self.free_blocks.add(idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks"""
        return len(self.free_blocks)
    
    # ===== Red-Black Tree Operations =====
    
    def _insert_node(self, index):
        """Insert index into RB tree"""
        new_node = RBNode(index)
        
        if not self.root:
            new_node.color = Color.BLACK
            self.root = new_node
            return
        
        parent = None
        current = self.root
        
        while current:
            parent = current
            if index < current.index:
                current = current.left
            elif index > current.index:
                current = current.right
            else:
                return
        
        new_node.parent = parent
        if index < parent.index:
            parent.left = new_node
        else:
            parent.right = new_node
        
        self._fix_insert(new_node)
    
    def _delete_node(self, index):
        """Delete index from RB tree"""
        node = self._find_node(self.root, index)
        if not node:
            return
        
        if node.left and node.right:
            successor = self._find_min(node.right)
            node.index = successor.index
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
        
        if node.color == Color.BLACK:
            if child:
                self._fix_delete(child)
            elif node.parent:
                self._fix_delete_null(node.parent, node == node.parent.left)
    
    def _find_node(self, node, index):
        """Find node with given index"""
        while node:
            if index == node.index:
                return node
            elif index < node.index:
                node = node.left
            else:
                node = node.right
        return None
    
    def _find_min(self, node):
        """Find minimum node in subtree"""
        while node.left:
            node = node.left
        return node
    
    def _rotate_left(self, node):
        """Left rotation"""
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
    
    def _rotate_right(self, node):
        """Right rotation"""
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
    
    def _fix_insert(self, node):
        """Fix RB properties after insertion"""
        while node.parent and node.parent.color == Color.RED:
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
        """Fix RB properties after deletion"""
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
    
    def _fix_delete_null(self, parent, is_left):
        """Fix when deleted node's child is None"""
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
    
    def _get_color(self, node):
        """Get color, treating None as BLACK"""
        return node.color if node else Color.BLACK
    
    def _count_recursive(self, node):
        """Recursively count nodes in tree"""
        if not node:
            return 0
        return 1 + self._count_recursive(node.left) + self._count_recursive(node.right)
