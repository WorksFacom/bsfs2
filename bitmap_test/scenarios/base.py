"""
Base class for benchmark scenarios.

`run(allocator_class, allocator_name, bitmap_size, **kwargs)` returns a metrics dict in the
`build_stress_metrics` shape. `show_progress` and `show_summary` kwargs silence output in batch runs.
"""

from __future__ import annotations

from abc import ABC, abstractmethod


class BenchmarkScenario(ABC):
    @abstractmethod
    def run(self, allocator_class, allocator_name, bitmap_size, **kwargs):
        raise NotImplementedError
