"""
Abstract base contract for all benchmark scenarios.

BenchmarkScenario:
- ABC with a single abstract method `run(allocator_class, allocator_name, bitmap_size, **kwargs)`.
- all scenarios return a metrics dict compatible with `build_stress_metrics`.
- `show_progress` and `show_summary` are optional kwargs to suppress output during batch runs.
"""

from __future__ import annotations

from abc import ABC, abstractmethod


class BenchmarkScenario(ABC):
    @abstractmethod
    def run(self, allocator_class, allocator_name, bitmap_size, **kwargs):
        raise NotImplementedError
