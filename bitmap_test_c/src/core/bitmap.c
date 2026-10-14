#include "bitmap.h"

#include <stdlib.h>
#include <string.h>

void bitmap_init(bitmap *bm, size_t size) {
    /* memset instead of calloc so every page faults in here, not inside a timed allocate */
    bm->data = (uint8_t *)malloc(size ? size : 1);
    memset(bm->data, 0, size ? size : 1);
    bm->size = size;
    bm->free_count = size;
}

void bitmap_destroy(bitmap *bm) {
    free(bm->data);
    bm->data = NULL;
    bm->size = 0;
    bm->free_count = 0;
}

void bitmap_set(bitmap *bm, size_t index, int value) {
    uint8_t old = bm->data[index];
    uint8_t nw = value ? 1 : 0;
    if (old == nw) {
        return;
    }
    if (old == 0) {
        bm->free_count -= 1;
    } else {
        bm->free_count += 1;
    }
    bm->data[index] = nw;
}

void bitmap_set_range(bitmap *bm, size_t start, size_t stop, int value) {
    uint8_t nw = value ? 1 : 0;
    for (size_t i = start; i < stop; i++) {
        uint8_t old = bm->data[i];
        if (old == nw) {
            continue;
        }
        if (old == 0) {
            bm->free_count -= 1;
        } else {
            bm->free_count += 1;
        }
        bm->data[i] = nw;
    }
}

size_t bitmap_count(const bitmap *bm, int value) {
    if (value == 0) {
        return bm->free_count;
    }
    if (value == 1) {
        return bm->size - bm->free_count;
    }
    size_t c = 0;
    for (size_t i = 0; i < bm->size; i++) {
        if (bm->data[i] == (uint8_t)value) {
            c++;
        }
    }
    return c;
}
