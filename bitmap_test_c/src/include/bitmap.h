/*
 * BitmapBits, port of core/base_allocator.py::BitmapBits.
 *
 * one byte per block (0/1), not packed bits, same as the Python model. `free_count` is kept up to
 * date on every write so counting is O(1).
 */
#ifndef BITMAP_H
#define BITMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t free_count;
} bitmap;

void bitmap_init(bitmap *bm, size_t size);
void bitmap_destroy(bitmap *bm);

static inline uint8_t bitmap_get(const bitmap *bm, size_t index) {
    return bm->data[index];
}

/* set one bit, adjusting free_count on 0<->1 transitions */
void bitmap_set(bitmap *bm, size_t index, int value);

/* slice assignment: set [start, stop) to value, adjusting free_count */
void bitmap_set_range(bitmap *bm, size_t start, size_t stop, int value);

/* count(value): for 0 returns free_count, for 1 returns size-free_count. */
size_t bitmap_count(const bitmap *bm, int value);

#endif /* BITMAP_H */
