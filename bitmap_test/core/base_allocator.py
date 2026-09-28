"""
Base classes for all bitmap allocators.

`BitmapBits` is a bytearray bitmap that keeps `free_count` up to date in __setitem__,
so free-count queries never scan. Slice assignment checks the length matches.

`BaseAllocator` is the contract: `allocate_blocks`, `deallocate_blocks`, `count_free_blocks`.
`validate_deallocation` rejects duplicates, out-of-range indices and double frees.
`rollback_bitmap_allocation` undoes a multi-block request that failed partway.
"""

from __future__ import annotations

from abc import ABC, abstractmethod


class BitmapBits:
    """Bitmap with an incrementally maintained free count."""

    __slots__ = ("_data", "size", "free_count")

    def __init__(self, size: int):
        self._data = bytearray(size)
        self.size = size
        self.free_count = size

    def __len__(self):
        return self.size

    def __iter__(self):
        return iter(self._data)

    def __getitem__(self, index):
        return self._data[index]

    def __setitem__(self, index, value):
        if isinstance(index, slice):
            start, stop, step = index.indices(self.size)
            old_values = self._data[index]
            new_values = bytes(1 if int(v) else 0 for v in value)
            if len(new_values) != len(range(start, stop, step)):
                raise ValueError("bitmap slice assignment length mismatch")
            for old, new in zip(old_values, new_values):
                if old == new:
                    continue
                if old == 0:
                    self.free_count -= 1
                else:
                    self.free_count += 1
            self._data[index] = new_values
            return

        old = self._data[index]
        new = 1 if int(value) else 0
        if old == new:
            return
        if old == 0:
            self.free_count -= 1
        else:
            self.free_count += 1
        self._data[index] = new

    def count(self, value):
        if value == 0:
            return self.free_count
        if value == 1:
            return self.size - self.free_count
        return self._data.count(value)


class BaseAllocator(ABC):
    """Common contract for bitmap allocators."""

    # allocators in one group must produce byte-identical bitmaps on a fixed workload (the validator
    # checks this). scatter allocators all return the N smallest free indices so they share a group;
    # extent allocators differ by policy and declare their own.
    CONSISTENCY_GROUP = "scatter"

    def __init__(self, size: int):
        self.bitmap = BitmapBits(size)
        self.size = size

    @property
    def free_blocks_remaining(self) -> int:
        return self.bitmap.free_count

    @abstractmethod
    def allocate_blocks(self, count: int):
        raise NotImplementedError

    @abstractmethod
    def deallocate_blocks(self, indices):
        raise NotImplementedError

    @abstractmethod
    def count_free_blocks(self) -> int:
        raise NotImplementedError

    def validate_deallocation(self, indices) -> bool:
        if len(set(indices)) != len(indices):
            return False
        for idx in indices:
            if idx < 0 or idx >= self.size:
                return False
            if self.bitmap[idx] == 0:
                return False
        return True

    def rollback_bitmap_allocation(self, allocated):
        for idx in allocated:
            self.bitmap[idx] = 0
