"""Core building blocks for bitmap benchmark architecture."""

from .base_allocator import BaseAllocator
from .registry import (
    StrategySpec,
    get_strategy,
    iter_strategies,
    list_strategies,
    register,
)

__all__ = [
    "BaseAllocator",
    "StrategySpec",
    "register",
    "get_strategy",
    "list_strategies",
    "iter_strategies",
]
