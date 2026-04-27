"""Fixed bitmap layout constants used across bitmap benchmark tests."""

from __future__ import annotations

DISK_SIZE_GIB = 250
PAGE_SIZE_KIB = 4
KIB_PER_GIB = 1024 * 1024

def bitmap_size_blocks_from_disk_gib(disk_size_gib: int, page_size_kib: int = PAGE_SIZE_KIB) -> int:
    if disk_size_gib <= 0:
        raise ValueError("disk_size_gib must be > 0")
    if page_size_kib <= 0:
        raise ValueError("page_size_kib must be > 0")
    return (disk_size_gib * KIB_PER_GIB) // page_size_kib


# 250 GiB disk / 4 KiB page => 65,536,000 pages (1 bit per page)
FIXED_BITMAP_SIZE_BLOCKS = bitmap_size_blocks_from_disk_gib(DISK_SIZE_GIB, PAGE_SIZE_KIB)
FIXED_BITMAP_SIZE_BITS = FIXED_BITMAP_SIZE_BLOCKS
FIXED_BITMAP_SIZE_BYTES = FIXED_BITMAP_SIZE_BITS // 8
