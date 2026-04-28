"""
Lightweight shared types used by scenarios and runners.

Exported types:
- `AllocatorFactory` — callable `(int) → allocator`, used to instantiate allocators by bitmap size.
- `MetricsDict` — alias for `Dict[str, float]`, the result type of any scenario run.
- `ScenarioResult` — immutable wrapper around the metrics dict.
- `StrategyDescriptor` — named tuple with `key`, `allocator_class`, and `name` as used by the registry.
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
