"""Automatic strategy registry for allocators."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Iterable, Type


@dataclass(frozen=True)
class StrategySpec:
    key: str
    allocator_class: Type
    display_name: str


_REGISTRY: Dict[str, StrategySpec] = {}


def register(key: str, display_name: str | None = None):
    """Decorator to register allocator classes by strategy key."""

    def _decorator(cls):
        spec = StrategySpec(
            key=key,
            allocator_class=cls,
            display_name=display_name or getattr(cls, "__name__", key),
        )
        _REGISTRY[key] = spec
        return cls

    return _decorator


def get_strategy(key: str) -> StrategySpec:
    return _REGISTRY[key]


def list_strategies() -> list[StrategySpec]:
    return [spec for _, spec in sorted(_REGISTRY.items(), key=lambda item: item[0])]


def iter_strategies() -> Iterable[StrategySpec]:
    for key in sorted(_REGISTRY.keys()):
        yield _REGISTRY[key]
