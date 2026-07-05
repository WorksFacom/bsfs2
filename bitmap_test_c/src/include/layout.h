/*
 * Bitmap layout constants derived from the simulated disk configuration.
 * Port of core/bitmap_layout.py.
 *
 * 250 GiB disk / 4 KiB page => 65,536,000 pages (1 bit per page).
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#define DISK_SIZE_GIB 250
#define PAGE_SIZE_KIB 4
#define KIB_PER_GIB (1024L * 1024L)

/* (disk_size_gib * KIB_PER_GIB) // page_size_kib; aborts on non-positive args. */
long bitmap_size_blocks_from_disk_gib(long disk_size_gib, long page_size_kib);

#define FIXED_BITMAP_SIZE_BLOCKS (((long)DISK_SIZE_GIB * KIB_PER_GIB) / PAGE_SIZE_KIB)

#endif /* LAYOUT_H */
