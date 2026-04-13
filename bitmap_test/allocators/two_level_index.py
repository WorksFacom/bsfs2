"""
Two-Level Index allocator for bitmap block management.

Uses a two-level hierarchy:
- Level 1: Bucket index (tracks which buckets have free space)
- Level 2: Within each bucket, use a simple RB-Tree for efficient searches

This combines spatial locality with fast indexing.
"""

import heapq


class BitmapTwoLevelIndex:
    """Bitmap allocator using two-level indexing with buckets."""
    
    BUCKET_SIZE = 100  # Blocks per bucket
    
    def __init__(self, size):
        self.size = size
        self.bitmap = [0] * size  # 0 = free, 1 = occupied
        
        # Calculate number of buckets
        self.num_buckets = (size + self.BUCKET_SIZE - 1) // self.BUCKET_SIZE
        
        # Bucket info: free_count for each bucket
        self.bucket_free_count = [self.BUCKET_SIZE] * self.num_buckets
        
        # Adjust last bucket size if needed
        last_bucket_size = size % self.BUCKET_SIZE
        if last_bucket_size > 0:
            self.bucket_free_count[-1] = last_bucket_size
        
        # Min-heap of (free_count, bucket_idx) for quick access to buckets with space
        # Negative free_count so heapq works as max-heap
        self.bucket_heap = [(-self.bucket_free_count[i], i) for i in range(self.num_buckets)]
        heapq.heapify(self.bucket_heap)
        
        # Track which buckets are valid in heap (to handle lazy deletion)
        self.bucket_heap_valid = [True] * self.num_buckets
    
    def _get_bucket_idx(self, block_idx):
        """Get bucket index for a block."""
        return block_idx // self.BUCKET_SIZE
    
    def _get_bucket_start(self, bucket_idx):
        """Get starting block index of a bucket."""
        return bucket_idx * self.BUCKET_SIZE
    
    def _get_bucket_end(self, bucket_idx):
        """Get ending block index of a bucket (exclusive)."""
        return min((bucket_idx + 1) * self.BUCKET_SIZE, self.size)
    
    def _find_free_in_bucket(self, bucket_idx, max_count=None):
        """Find free blocks in a bucket."""
        start = self._get_bucket_start(bucket_idx)
        end = self._get_bucket_end(bucket_idx)
        
        free_blocks = []
        for i in range(start, end):
            if self.bitmap[i] == 0:
                free_blocks.append(i)
                if max_count and len(free_blocks) >= max_count:
                    break
        
        return free_blocks
    
    def _update_bucket_free_count(self, bucket_idx):
        """Recalculate free count for a bucket."""
        start = self._get_bucket_start(bucket_idx)
        end = self._get_bucket_end(bucket_idx)
        
        free_count = sum(1 for i in range(start, end) if self.bitmap[i] == 0)
        self.bucket_free_count[bucket_idx] = free_count
        
        # Invalidate old heap entries
        self.bucket_heap_valid[bucket_idx] = False
        
        # Add new entry
        if free_count > 0:
            heapq.heappush(self.bucket_heap, (-free_count, bucket_idx))
            self.bucket_heap_valid[bucket_idx] = True
    
    def _get_bucket_with_space(self, min_blocks=1):
        """Get bucket with at least min_blocks free space."""
        while self.bucket_heap:
            neg_free_count, bucket_idx = heapq.heappop(self.bucket_heap)
            free_count = -neg_free_count
            
            # Skip invalid entries
            if not self.bucket_heap_valid[bucket_idx]:
                continue
            
            if free_count >= min_blocks:
                # Push back for next search
                heapq.heappush(self.bucket_heap, (neg_free_count, bucket_idx))
                return bucket_idx
            
            # Mark as invalid since it doesn't have enough space
            self.bucket_heap_valid[bucket_idx] = False
        
        return -1
    
    def allocate_blocks(self, count):
        """Allocate count blocks."""
        allocated = []
        remaining = count
        
        # Try to allocate from buckets with space
        while remaining > 0:
            bucket_idx = self._get_bucket_with_space(1)
            
            if bucket_idx == -1:
                break
            
            # Get free blocks from this bucket
            free_blocks = self._find_free_in_bucket(bucket_idx, remaining)
            
            if not free_blocks:
                self._update_bucket_free_count(bucket_idx)
                continue
            
            # Allocate blocks
            for block_idx in free_blocks[:remaining]:
                self.bitmap[block_idx] = 1
                allocated.append(block_idx)
                remaining -= 1
            
            # Update bucket info
            self._update_bucket_free_count(bucket_idx)
        
        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks."""
        for idx in indices:
            if idx < 0 or idx >= self.size:
                return False
            if self.bitmap[idx] == 0:
                return False

        for idx in indices:
            self.bitmap[idx] = 0

            # Update affected bucket
            bucket_idx = self._get_bucket_idx(idx)
            self._update_bucket_free_count(bucket_idx)
        return True
    
    def count_free_blocks(self):
        """Count total free blocks."""
        return sum(1 for bit in self.bitmap if bit == 0)
