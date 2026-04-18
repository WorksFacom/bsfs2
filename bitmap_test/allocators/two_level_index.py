"""
Two-Level Index allocator for bitmap block management.

Uses a two-level hierarchy:
- Level 1: Bucket metadata (free count per bucket)
- Level 2: Sequential scan within the selected bucket
"""


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
    
    def _get_bucket_with_space(self, min_blocks=1):
        """Get first bucket (lowest index) with at least min_blocks free space."""
        for bucket_idx, free_count in enumerate(self.bucket_free_count):
            if free_count >= min_blocks:
                return bucket_idx
        return -1

    def _find_first_free_block(self):
        """Find first free block using bucket metadata."""
        bucket_idx = self._get_bucket_with_space(1)
        if bucket_idx == -1:
            return -1

        start = self._get_bucket_start(bucket_idx)
        end = self._get_bucket_end(bucket_idx)
        for idx in range(start, end):
            if self.bitmap[idx] == 0:
                return idx
        return -1
    
    def allocate_blocks(self, count):
        """Allocate count blocks."""
        if count <= 0:
            return []

        allocated = []

        for _ in range(count):
            block_idx = self._find_first_free_block()
            if block_idx == -1:
                # Rollback to preserve transactional behavior.
                for idx in allocated:
                    self.bitmap[idx] = 0
                for bucket_idx in {self._get_bucket_idx(i) for i in allocated}:
                    self._update_bucket_free_count(bucket_idx)
                return []

            self.bitmap[block_idx] = 1
            allocated.append(block_idx)
            self._update_bucket_free_count(self._get_bucket_idx(block_idx))

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

            # Update affected bucket
            bucket_idx = self._get_bucket_idx(idx)
            self._update_bucket_free_count(bucket_idx)
        return True
    
    def count_free_blocks(self):
        """Count total free blocks."""
        return sum(1 for bit in self.bitmap if bit == 0)
