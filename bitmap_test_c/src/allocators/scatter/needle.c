/*
 * sequential allocator with a first-free needle cursor. port of allocators/scatter/needle_sequential.py.
 */
#include <stdlib.h>

#include "allocators.h"

typedef struct {
    allocator base;
    long needle;
} needle_alloc;

static long needle_find_next_free(needle_alloc *a, long start_idx) {
    long size = a->base.size;
    if (size == 0) {
        return -1;
    }
    long idx = start_idx % size;
    long scanned = 0;
    while (scanned < size) {
        if (bitmap_get(&a->base.bm, (size_t)idx) == 0) {
            return idx;
        }
        idx = (idx + 1) % size;
        scanned += 1;
    }
    return -1;
}

static long needle_allocate(allocator *self, long count, long *out) {
    needle_alloc *a = (needle_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    long size = self->size;
    long start = (a->needle < size) ? a->needle : 0;
    long idx = start;
    long scanned = 0;
    long n = 0;

    while (scanned < size && n < count) {
        if (bitmap_get(&self->bm, (size_t)idx) == 0) {
            out[n++] = idx;
        }
        idx = (idx + 1) % size;
        scanned += 1;
    }

    if (n != count) {
        return 0;
    }

    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)out[i], 1);
    }

    long next_free = needle_find_next_free(a, idx);
    a->needle = (next_free != -1) ? next_free : size;
    return n;
}

static bool needle_deallocate(allocator *self, const long *indices, long n) {
    needle_alloc *a = (needle_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    long freed_min = -1;
    int has_min = 0;
    for (long i = 0; i < n; i++) {
        if (!has_min || indices[i] < freed_min) {
            freed_min = indices[i];
            has_min = 1;
        }
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
    }
    if (has_min) {
        if (a->needle >= self->size || freed_min < a->needle) {
            a->needle = freed_min;
        }
    }
    return true;
}

static long needle_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double needle_aux_mem(allocator *self) {
    (void)self;
    return (double)sizeof(long);
}

static void needle_destroy(allocator *self) {
    allocator_base_destroy(self);
    free(self);
}

allocator *needle_create(long size) {
    needle_alloc *a = (needle_alloc *)malloc(sizeof(needle_alloc));
    allocator_base_init(&a->base, size);
    a->needle = 0;
    a->base.allocate = needle_allocate;
    a->base.deallocate = needle_deallocate;
    a->base.count_free = needle_count_free;
    a->base.aux_mem_bytes = needle_aux_mem;
    a->base.destroy = needle_destroy;
    return &a->base;
}
