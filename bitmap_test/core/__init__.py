"""Core building blocks for bitmap benchmark architecture."""

from .base_allocator import BaseAllocator
from .registry import (
    StrategySpec,
    get_strategy,
    iter_strategies,
    list_strategies,
    register,
)
from .stats_config import StatBenchmarkConfig
from .stats import generate_seeds

__all__ = [
    "BaseAllocator",
    "StrategySpec",
    "StatBenchmarkConfig",
    "generate_seeds",
    "register",
    "get_strategy",
    "list_strategies",
    "iter_strategies",
]
