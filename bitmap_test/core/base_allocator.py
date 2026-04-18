"""Shared allocator base class and validation helpers."""

from __future__ import annotations

from abc import ABC, abstractmethod


class BaseAllocator(ABC):
    """Common contract for bitmap allocators."""

    def __init__(self, size: int):
        self.bitmap = [0] * size
        self.size = size

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
