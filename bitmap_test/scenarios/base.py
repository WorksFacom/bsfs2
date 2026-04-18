"""Base scenario contract."""

from __future__ import annotations

from abc import ABC, abstractmethod


class BenchmarkScenario(ABC):
    @abstractmethod
    def run(self, allocator_class, allocator_name, bitmap_size, **kwargs):
        raise NotImplementedError
