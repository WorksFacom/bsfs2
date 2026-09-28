"""
Two-level index allocator: buckets of blocks with a free count each.

Allocation starts at `last_bucket_idx`, skips buckets with free_count == 0 without touching their
blocks, and scans forward through the rest in one pass for all `count` blocks, updating bucket
counters as it goes. Deallocation clears bits, bumps the affected bucket counters, and resets the
cursor to 0 so buckets freed behind it get found again.
"""

from core import BaseAllocator, register


@register("twolevel", display_name="Two-Level Index")
class BitmapTwoLevelIndex(BaseAllocator):
    BUCKET_SIZE = 100  # blocks per bucket
    
    def __init__(self, size):
        super().__init__(size)

        self.num_buckets = (size + self.BUCKET_SIZE - 1) // self.BUCKET_SIZE

        self.bucket_free_count = [self.BUCKET_SIZE] * self.num_buckets

        # last bucket may be short
        last_bucket_size = size % self.BUCKET_SIZE
        if last_bucket_size > 0:
            self.bucket_free_count[-1] = last_bucket_size

        # saves rescanning from bucket 0 every time
        self.last_bucket_idx = 0
        
    def _get_bucket_idx(self, block_idx):
        return block_idx // self.BUCKET_SIZE
    
    def _get_bucket_start(self, bucket_idx):
        return bucket_idx * self.BUCKET_SIZE
    
    def _get_bucket_end(self, bucket_idx):
        """Exclusive."""
        return min((bucket_idx + 1) * self.BUCKET_SIZE, self.size)
    
    def _get_bucket_with_space(self, min_blocks=1):
        """First bucket with at least min_blocks free, starting at the cursor and wrapping."""
        for i in range(self.last_bucket_idx, self.num_buckets):
            if self.bucket_free_count[i] >= min_blocks:
                return i

        for i in range(0, self.last_bucket_idx):
            if self.bucket_free_count[i] >= min_blocks:
                return i

        return -1

    def _find_first_free_block(self):
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
        """Single forward scan across buckets for all `count` blocks."""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []
        bucket_free_count = self.bucket_free_count
        bitmap = self.bitmap
        bucket_idx = self.last_bucket_idx

        while bucket_idx < self.num_buckets and bucket_free_count[bucket_idx] == 0:
            bucket_idx += 1

        if bucket_idx == self.num_buckets:
            # wrap around
            bucket_idx = 0
            while bucket_idx < self.last_bucket_idx and bucket_free_count[bucket_idx] == 0:
                bucket_idx += 1
            if bucket_idx == self.last_bucket_idx:
                return []

        pos = self._get_bucket_start(bucket_idx)
        bucket_end = self._get_bucket_end(bucket_idx)

        while len(allocated) < count:
            if pos >= bucket_end:
                bucket_idx += 1
                while bucket_idx < self.num_buckets and bucket_free_count[bucket_idx] == 0:
                    bucket_idx += 1
                if bucket_idx >= self.num_buckets:
                    break
                pos = self._get_bucket_start(bucket_idx)
                bucket_end = self._get_bucket_end(bucket_idx)

            if bitmap[pos] == 0:
                allocated.append(pos)
                bitmap[pos] = 1
                bucket_free_count[bucket_idx] -= 1

            pos += 1

        if len(allocated) < count:
            for idx in allocated:
                bitmap[idx] = 0
                bucket_free_count[self._get_bucket_idx(idx)] += 1
            return []

        self.last_bucket_idx = bucket_idx
        return allocated
    
    def deallocate_blocks(self, indices):
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0

            bucket_idx = self._get_bucket_idx(idx)
            self.bucket_free_count[bucket_idx] += 1

        # freed blocks may be behind the cursor
        self.last_bucket_idx = 0
        return True
    
    def count_free_blocks(self):
        return self.free_blocks_remaining
