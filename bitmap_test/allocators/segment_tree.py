"""
Segment Tree allocator for bitmap block management.

Uses a proper segment tree to track free blocks in ranges.
Supports efficient range queries for finding contiguous free blocks.
"""


class SegmentTreeNode:
    """Node in a segment tree."""
    
    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.free_count = end - start  # Count of free blocks in this range
        self.left = None
        self.right = None


class BitmapSegmentTree:
    """Bitmap allocator using Segment Tree."""
    
    def __init__(self, size):
        self.size = size
        self.bitmap = [0] * size  # 0 = free, 1 = occupied
        self.root = self._build_tree(0, size - 1) if size > 0 else None
    
    def _build_tree(self, start, end):
        """Build segment tree recursively."""
        node = SegmentTreeNode(start, end)
        
        if start == end:
            node.free_count = 1
            return node
        
        mid = (start + end) // 2
        node.left = self._build_tree(start, mid)
        node.right = self._build_tree(mid + 1, end)
        node.free_count = node.left.free_count + node.right.free_count
        
        return node
    
    def _update_point(self, node, idx, is_free):
        """Update a single point in the tree."""
        if node is None:
            return
        
        if node.start == node.end:
            node.free_count = 1 if is_free else 0
            return
        
        mid = (node.start + node.end) // 2
        
        if idx <= mid:
            self._update_point(node.left, idx, is_free)
        else:
            self._update_point(node.right, idx, is_free)
        
        left_free = node.left.free_count if node.left else 0
        right_free = node.right.free_count if node.right else 0
        node.free_count = left_free + right_free
    
    def _find_free(self, node, count):
        """Find first position with 'count' free blocks in range."""
        if node is None or node.free_count < count:
            return -1
        
        if node.start == node.end:
            return node.start if self.bitmap[node.start] == 0 else -1
        
        # Try left subtree first
        if node.left and node.left.free_count >= count:
            result = self._find_free(node.left, count)
            if result != -1:
                return result
        
        # Try right subtree
        if node.right and node.right.free_count >= count:
            result = self._find_free(node.right, count)
            if result != -1:
                return result
        
        return -1
    
    def allocate_blocks(self, count):
        """Allocate count blocks."""
        if count <= 0:
            return []
        if not self.root:
            return []

        allocated = []
        
        # Try to find and allocate count blocks atomically.
        for _ in range(count):
            idx = self._find_free(self.root, 1)
            
            if idx == -1:
                for allocated_idx in allocated:
                    self.bitmap[allocated_idx] = 0
                    self._update_point(self.root, allocated_idx, True)
                return []
            
            # Mark as occupied
            self.bitmap[idx] = 1
            self._update_point(self.root, idx, False)
            allocated.append(idx)
        
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
            self._update_point(self.root, idx, True)
        return True
    
    def count_free_blocks(self):
        """Count total free blocks."""
        return self.root.free_count if self.root else 0
