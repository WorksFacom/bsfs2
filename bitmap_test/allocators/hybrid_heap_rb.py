"""
Hybrid Heap + RB-Tree allocator for bitmap block allocation
- Min-Heap: O(1) minimum finding for fast allocation
- RB-Tree: O(log n) insertion/deletion for efficient deallocation
"""

import heapq
from core import BaseAllocator, register

class Color:
    RED = 0
    BLACK = 1


class RBNode:
    def __init__(self, key):
        self.key = key
        self.color = Color.RED
        self.left = None
        self.right = None
        self.parent = None


class HybridRBTreeForDealloc:
    """RB-Tree optimized for deallocation tracking"""
    
    def __init__(self):
        self.root = None
    
    def insert(self, key):
        if not self.root:
            self.root = RBNode(key)
            self.root.color = Color.BLACK
            return
        
        node = self.root
        while True:
            if key < node.key:
                if node.left:
                    node = node.left
                else:
                    node.left = RBNode(key)
                    node.left.parent = node
                    self._fix_insert(node.left)
                    return
            elif key > node.key:
                if node.right:
                    node = node.right
                else:
                    node.right = RBNode(key)
                    node.right.parent = node
                    self._fix_insert(node.right)
                    return
            else:
                return
    
    def delete(self, key):
        node = self._find(self.root, key)
        if not node:
            return
        
        if node.left and node.right:
            successor = self._min_node(node.right)
            node.key = successor.key
            node = successor
        
        child = node.left if node.left else node.right
        
        if node.color == Color.BLACK:
            if child:
                child.color = Color.BLACK
            self._fix_delete(node)
        
        if node.parent:
            if node.parent.left == node:
                node.parent.left = child
            else:
                node.parent.right = child
            if child:
                child.parent = node.parent
        else:
            self.root = child
            if self.root:
                self.root.color = Color.BLACK
    
    def _find(self, node, key):
        while node:
            if key == node.key:
                return node
            elif key < node.key:
                node = node.left
            else:
                node = node.right
        return None
    
    def _min_node(self, node):
        while node.left:
            node = node.left
        return node
    
    def _fix_insert(self, node):
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
        while node != self.root and self._get_color(node) == Color.BLACK:
            if node == node.parent.left:
                sibling = node.parent.right
                if self._get_color(sibling) == Color.RED:
                    sibling.color = Color.BLACK
                    node.parent.color = Color.RED
                    self._rotate_left(node.parent)
                    sibling = node.parent.right
                
                if (self._get_color(sibling.left) == Color.BLACK and
                    self._get_color(sibling.right) == Color.BLACK):
                    sibling.color = Color.RED
                    node = node.parent
                else:
                    if self._get_color(sibling.right) == Color.BLACK:
                        sibling.left.color = Color.BLACK
                        sibling.color = Color.RED
                        self._rotate_right(sibling)
                        sibling = node.parent.right
                    
                    sibling.color = node.parent.color
                    node.parent.color = Color.BLACK
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
                
                if (self._get_color(sibling.left) == Color.BLACK and
                    self._get_color(sibling.right) == Color.BLACK):
                    sibling.color = Color.RED
                    node = node.parent
                else:
                    if self._get_color(sibling.left) == Color.BLACK:
                        sibling.right.color = Color.BLACK
                        sibling.color = Color.RED
                        self._rotate_left(sibling)
                        sibling = node.parent.left
                    
                    sibling.color = node.parent.color
                    node.parent.color = Color.BLACK
                    sibling.left.color = Color.BLACK
                    self._rotate_right(node.parent)
                    node = self.root
    
    def _get_color(self, node):
        return Color.BLACK if not node else node.color
    
    def _rotate_left(self, node):
        right = node.right
        node.right = right.left
        if right.left:
            right.left.parent = node
        right.parent = node.parent
        if not node.parent:
            self.root = right
        elif node == node.parent.left:
            node.parent.left = right
        else:
            node.parent.right = right
        right.left = node
        node.parent = right
    
    def _rotate_right(self, node):
        left = node.left
        node.left = left.right
        if left.right:
            left.right.parent = node
        left.parent = node.parent
        if not node.parent:
            self.root = left
        elif node == node.parent.right:
            node.parent.right = left
        else:
            node.parent.left = left
        left.right = node
        node.parent = left
    
    def contains(self, key):
        return self._find(self.root, key) is not None


@register("hybrid", display_name="Hybrid Heap+RB")
class BitmapHybridHeapRB(BaseAllocator):
    """Hybrid Heap + RB-Tree Bitmap Allocator
    
    Strategy:
    - Min-Heap: Fast allocation O(1) to find minimum
    - RB-Tree: Fast deallocation O(log n) with balanced structure
    - Position map: Tracks heap positions for efficient removal
    """
    
    def __init__(self, size, heap_order=256):
        super().__init__(size)
        self.heap_order = heap_order
        
        # Min-Heap for allocation
        self.heap = list(range(size))
        heapq.heapify(self.heap)
        self.position_map = {i: i for i in range(size)}
        
        # RB-Tree for deallocation
        self.rbtree = HybridRBTreeForDealloc()
        for i in range(size):
            self.rbtree.insert(i)
    
    def find_free_block(self):
        """Find minimum free block via heap (O(1))"""
        while self.heap and self.bitmap[self.heap[0]] == 1:
            heapq.heappop(self.heap)
        if self.heap:
            return self.heap[0]
        return -1
    
    def allocate_blocks(self, count):
        """Allocate blocks using heap for fast finding"""
        if count <= 0:
            return []

        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            if idx == -1:
                # Rollback
                for allocated_idx in allocated:
                    self.bitmap[allocated_idx] = 0
                    heapq.heappush(self.heap, allocated_idx)
                return []

            heapq.heappop(self.heap)
            self.bitmap[idx] = 1
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks using RB-Tree for fast insertion"""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            heapq.heappush(self.heap, idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks"""
        return sum(1 for b in self.bitmap if b == 0)
    
    # ===== Internal Operations =====
    
    def _insert_key(self, key):
        """Insert into both heap and RB-Tree (for deallocation)"""
        # Add to RB-Tree for fast deallocation later
        self.rbtree.insert(key)
        
        # Add to heap for fast allocation
        if key not in self.position_map:
            pos = len(self.heap)
            self.heap.append(key)
            self.position_map[key] = pos
            self._sift_up(pos)
    
    def _delete_key(self, key):
        """Remove from both heap and RB-Tree"""
        # Remove from RB-Tree
        self.rbtree.delete(key)
        
        # Remove from heap
        if key in self.position_map:
            pos = self.position_map[key]
            last_key = self.heap[-1]
            
            if key != last_key:
                self.heap[pos] = last_key
                self.position_map[last_key] = pos
                
                # Restore heap property
                if pos > 0 and self.heap[pos] < self.heap[(pos - 1) // 2]:
                    self._sift_up(pos)
                else:
                    self._sift_down(pos)
            
            self.heap.pop()
            del self.position_map[key]
    
    def _sift_up(self, pos):
        """Restore min-heap property upward"""
        key = self.heap[pos]
        while pos > 0:
            parent_pos = (pos - 1) // 2
            parent_key = self.heap[parent_pos]
            
            if key < parent_key:
                self.heap[pos] = parent_key
                self.position_map[parent_key] = pos
                pos = parent_pos
            else:
                break
        
        self.heap[pos] = key
        self.position_map[key] = pos
    
    def _sift_down(self, pos):
        """Restore min-heap property downward"""
        key = self.heap[pos]
        size = len(self.heap)
        
        while True:
            smallest = pos
            left = 2 * pos + 1
            right = 2 * pos + 2
            
            if left < size and self.heap[left] < self.heap[smallest]:
                smallest = left
            
            if right < size and self.heap[right] < self.heap[smallest]:
                smallest = right
            
            if smallest != pos:
                self.heap[pos] = self.heap[smallest]
                self.position_map[self.heap[pos]] = pos
                pos = smallest
            else:
                break
        
        self.heap[pos] = key
        self.position_map[key] = pos
