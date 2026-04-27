"""Core building blocks for bitmap benchmark architecture."""

from .base_allocator import BaseAllocator
from .bitmap_layout import (
    DISK_SIZE_GIB,
    FIXED_BITMAP_SIZE_BITS,
    FIXED_BITMAP_SIZE_BLOCKS,
    FIXED_BITMAP_SIZE_BYTES,
    PAGE_SIZE_KIB,
    bitmap_size_blocks_from_disk_gib,
)
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
    "DISK_SIZE_GIB",
    "PAGE_SIZE_KIB",
    "FIXED_BITMAP_SIZE_BLOCKS",
    "FIXED_BITMAP_SIZE_BITS",
    "FIXED_BITMAP_SIZE_BYTES",
    "bitmap_size_blocks_from_disk_gib",
    "StrategySpec",
    "StatBenchmarkConfig",
    "generate_seeds",
    "register",
    "get_strategy",
    "list_strategies",
    "iter_strategies",
]
