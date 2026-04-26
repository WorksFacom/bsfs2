"""Configuration model for statistical benchmark runs."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class StatBenchmarkConfig:
    num_files: int = 100
    file_size_mib: int = 10
    block_size_kib: int = 4
    bitmap_multiplier: int = 2
    num_seeds: int = 1000
    seed_base: int = 42
    mode: str = "mixed_7030"  # allocation_only | mixed_7030
    num_operations: int | None = None
    output_dir: str = "."

    @property
    def blocks_per_file(self) -> int:
        return (self.file_size_mib * 1024) // self.block_size_kib

    @property
    def target_blocks(self) -> int:
        return self.num_files * self.blocks_per_file

    @property
    def bitmap_size(self) -> int:
        return self.target_blocks * self.bitmap_multiplier

    @property
    def resolved_num_operations(self) -> int:
        if self.num_operations is not None:
            return self.num_operations
        if self.mode == "allocation_only":
            return self.num_files
        return 100000

    def validate(self):
        if self.num_files <= 0:
            raise ValueError("num_files must be > 0")
        if self.file_size_mib <= 0:
            raise ValueError("file_size_mib must be > 0")
        if self.block_size_kib <= 0:
            raise ValueError("block_size_kib must be > 0")
        if self.bitmap_multiplier <= 0:
            raise ValueError("bitmap_multiplier must be > 0")
        if self.num_seeds <= 0:
            raise ValueError("num_seeds must be > 0")
        if self.resolved_num_operations <= 0:
            raise ValueError("num_operations must be > 0")
        if self.mode not in {"allocation_only", "mixed_7030"}:
            raise ValueError("mode must be 'allocation_only' or 'mixed_7030'")
