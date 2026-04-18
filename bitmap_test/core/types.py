"""Shared lightweight types used by scenarios and runners."""

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
