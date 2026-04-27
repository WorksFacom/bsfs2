"""
Two-Level Index allocator for bitmap block management.

Uses a two-level hierarchy:
- Level 1: Bucket metadata (free count per bucket)
- Level 2: Sequential scan within the selected bucket
"""

from core import BaseAllocator, register


@register("twolevel", display_name="Two-Level Index")
class BitmapTwoLevelIndex(BaseAllocator):
    """Bitmap allocator using two-level indexing with buckets."""
    
    BUCKET_SIZE = 100  # Blocks per bucket
    
    def __init__(self, size):
        super().__init__(size)

        # Calculate number of buckets
        self.num_buckets = (size + self.BUCKET_SIZE - 1) // self.BUCKET_SIZE

        # Bucket info: free_count for each bucket
        self.bucket_free_count = [self.BUCKET_SIZE] * self.num_buckets

        # Adjust last bucket size if needed
        last_bucket_size = size % self.BUCKET_SIZE
        if last_bucket_size > 0:
            self.bucket_free_count[-1] = last_bucket_size

        # Cursor to avoid rescanning from bucket 0 every time
        self.last_bucket_idx = 0
        
    def _get_bucket_idx(self, block_idx):
        """Get bucket index for a block."""
        return block_idx // self.BUCKET_SIZE
    
    def _get_bucket_start(self, bucket_idx):
        """Get starting block index of a bucket."""
        return bucket_idx * self.BUCKET_SIZE
    
    def _get_bucket_end(self, bucket_idx):
        """Get ending block index of a bucket (exclusive)."""
        return min((bucket_idx + 1) * self.BUCKET_SIZE, self.size)
    
    def _get_bucket_with_space(self, min_blocks=1):
        """Get first bucket with at least min_blocks free space, starting from cursor."""
        # Search forward from last position
        for i in range(self.last_bucket_idx, self.num_buckets):
            if self.bucket_free_count[i] >= min_blocks:
                return i

        # If not found forward, search from beginning to cursor
        for i in range(0, self.last_bucket_idx):
            if self.bucket_free_count[i] >= min_blocks:
                return i

        return -1

    def _find_first_free_block(self):
        """Find first free block using bucket metadata."""
        bucket_idx = self._get_bucket_with_space(1)
        if bucket_idx == -1:
            return -1

        self.last_bucket_idx = bucket_idx

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
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            block_idx = self._find_first_free_block()
            if block_idx == -1:
                return []

            self.bitmap[block_idx] = 1
            allocated.append(block_idx)
            bucket_idx = self._get_bucket_idx(block_idx)
            self.bucket_free_count[bucket_idx] -= 1

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks."""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0

            # Update affected bucket
            bucket_idx = self._get_bucket_idx(idx)
            self.bucket_free_count[bucket_idx] += 1

        # Reset cursor to ensure we check earlier buckets first
        self.last_bucket_idx = 0
        return True
    
    def count_free_blocks(self):
        """Count total free blocks."""
        return self.free_blocks_remaining
