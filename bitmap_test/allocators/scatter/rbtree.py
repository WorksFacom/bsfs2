"""
Lazy red-black tree allocator.

Recycled indices live in an RB tree. Allocation takes the smaller of the tree minimum and
`next_fresh` (first block of the never-used range), removing the node or advancing the cursor.
Frees go into the tree and never move the cursor.
"""

from enum import IntEnum

from core import BaseAllocator, register


class Color(IntEnum):
    RED = 0
    BLACK = 1


class RBNode:
    def __init__(self, index):
        self.index = index
        self.color = Color.RED
        self.left = None
        self.right = None
        self.parent = None


@register("rb", display_name="Red-Black Tree")
class BitmapRB(BaseAllocator):
    def __init__(self, size):
        super().__init__(size)
        self.root = None
        self.next_fresh = 0
    
    def _select_min_free(self):
        """Smallest free index across the tree and the fresh cursor, O(log n)."""
        recycled_min_node = self._find_min(self.root) if self.root else None
        recycled_min = recycled_min_node.index if recycled_min_node else None
        fresh_min = self.next_fresh if self.next_fresh < self.size else None
        
        if recycled_min is None:
            return fresh_min
        if fresh_min is None:
            return recycled_min
        return recycled_min if recycled_min < fresh_min else fresh_min
    
    def allocate_blocks(self, count):
        """O(count log n)."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self._select_min_free()
            if idx == -1:
                return []
            
            if self.root and idx == self._find_min(self.root).index:
                self._delete_node(idx)
            else:
                self.next_fresh += 1
            
            self.bitmap[idx] = 1
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """O(count log n)."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self._insert_node(idx)
        return True
    
    def count_free_blocks(self):
        return self.free_blocks_remaining
    
    # ===== red-black tree =====
    
    def _insert_node(self, index):
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
        while node:
            if index == node.index:
                return node
            elif index < node.index:
                node = node.left
            else:
                node = node.right
        return None
    
    def _find_min(self, node):
        """Leftmost node of the subtree, None if `node` is None."""
        if not node:
            return None
        while node.left:
            node = node.left
        return node
    
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
    
    def _fix_delete_null(self, parent, is_left):
        """Delete fixup for when the removed node's child is None."""
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
        """None counts as BLACK."""
        return node.color if node else Color.BLACK
