"""
Small shared types for scenarios and runners.

- `AllocatorFactory`: `(int) -> allocator`, builds an allocator for a bitmap size.
- `MetricsDict`: `Dict[str, float]`, what every scenario run returns.
- `ScenarioResult`: immutable wrapper around a metrics dict.
- `StrategyDescriptor`: named tuple of `key`, `allocator_class`, `name`, as the registry uses.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Dict, Type


AllocatorFactory = Callable[[int], object]
MetricsDict = Dict[str, float]


@dataclass(frozen=True)
class ScenarioResult:
    metrics: dict


@dataclass(frozen=True)
class StrategyDescriptor:
    key: str
    allocator_class: Type
    name: str
