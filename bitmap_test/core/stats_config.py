"""Configuration model for statistical benchmark runs."""

from __future__ import annotations

from dataclasses import dataclass

from .bitmap_layout import DISK_SIZE_GIB, PAGE_SIZE_KIB, bitmap_size_blocks_from_disk_gib


@dataclass(frozen=True)
class StatBenchmarkConfig:
    num_files: int = 100
    file_size_mib: int = 10
    block_size_kib: int = 4
    disk_size_gib: int = DISK_SIZE_GIB
    bitmap_size_override: int | None = None
    num_seeds: int = 1000
    seed_base: int = 42
    mode: str = "mixed_7030"  # allocation_only | mixed_7030 | multisize_pareto
    num_operations: int | None = None
    output_dir: str = "."
    workload_label: str = "default"

    @property
    def blocks_per_file(self) -> int:
        return (self.file_size_mib * 1024) // self.block_size_kib

    @property
    def target_blocks(self) -> int:
        return self.num_files * self.blocks_per_file

    @property
    def bitmap_size(self) -> int:
        if self.bitmap_size_override is not None:
            return self.bitmap_size_override
        return bitmap_size_blocks_from_disk_gib(self.disk_size_gib, PAGE_SIZE_KIB)

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
        if self.disk_size_gib <= 0:
            raise ValueError("disk_size_gib must be > 0")
        if self.bitmap_size_override is not None and self.bitmap_size_override <= 0:
            raise ValueError("bitmap_size_override must be > 0")
        if self.target_blocks > self.bitmap_size:
            raise ValueError("target workload exceeds bitmap capacity")
        if self.num_seeds <= 0:
            raise ValueError("num_seeds must be > 0")
        if self.resolved_num_operations <= 0:
            raise ValueError("num_operations must be > 0")
        if self.mode not in {"allocation_only", "mixed_7030", "multisize_pareto"}:
            raise ValueError("mode must be 'allocation_only', 'mixed_7030', or 'multisize_pareto'")
