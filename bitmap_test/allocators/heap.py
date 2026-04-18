"""
Min-Heap implementation for bitmap block allocation
"""

from core import BaseAllocator, register


@register("heap", display_name="Min-Heap")
class BitmapHeap(BaseAllocator):
    """Min-Heap indexed Bitmap Allocator"""
    
    def __init__(self, size):
        """Initialize bitmap of size blocks, all free (0)"""
        super().__init__(size)
        self.heap = list(range(size))
        self.position = {i: i for i in range(size)}
        
        for i in range((size - 2) // 2, -1, -1):
            self._heapify_down(i)
    
    def find_free_block(self):
        """Find minimum free index - O(1)"""
        if not self.heap:
            return -1
        return self.heap[0]
    
    def allocate_blocks(self, count):
        """Allocate count free blocks - O(count × log n)"""
        if count <= 0:
            return []

        allocated = []

        for _ in range(count):
            if not self.heap:
                for allocated_idx in allocated:
                    self._insert_to_heap(allocated_idx)
                    self.bitmap[allocated_idx] = 0
                return []

            idx = self.heap[0]
            self.bitmap[idx] = 1
            self._remove_min()
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks - O(count × log n)"""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self._insert_to_heap(idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks"""
        return len(self.heap)
    
    # ===== Min-Heap Operations =====
    
    def _parent(self, i):
        return (i - 1) // 2
    
    def _left_child(self, i):
        return 2 * i + 1
    
    def _right_child(self, i):
        return 2 * i + 2
    
    def _swap(self, i, j):
        """Swap two elements and update position map"""
        self.heap[i], self.heap[j] = self.heap[j], self.heap[i]
        self.position[self.heap[i]] = i
        self.position[self.heap[j]] = j
    
    def _heapify_up(self, i):
        """Move element up to maintain heap property"""
        while i > 0 and self.heap[self._parent(i)] > self.heap[i]:
            parent_idx = self._parent(i)
            self._swap(i, parent_idx)
            i = parent_idx
    
    def _heapify_down(self, i):
        """Move element down to maintain heap property"""
        smallest = i
        
        while True:
            left = self._left_child(i)
            right = self._right_child(i)
            
            if left < len(self.heap) and self.heap[left] < self.heap[smallest]:
                smallest = left
            
            if right < len(self.heap) and self.heap[right] < self.heap[smallest]:
                smallest = right
            
            if smallest != i:
                self._swap(i, smallest)
                i = smallest
            else:
                break
    
    def _insert_to_heap(self, index):
        """Insert index into heap"""
        pos = len(self.heap)
        self.heap.append(index)
        self.position[index] = pos
        self._heapify_up(pos)
    
    def _remove_min(self):
        """Remove minimum (root) from heap"""
        if not self.heap:
            return None
        
        min_val = self.heap[0]
        last_val = self.heap.pop()
        
        if self.heap:
            self.heap[0] = last_val
            self.position[last_val] = 0
            self._heapify_down(0)
        
        del self.position[min_val]
        return min_val
