/*
 * baseline allocator, linear bitmap scan with no index. port of allocators/scatter/baseline.py.
 */
#include <stdlib.h>

#include "allocators.h"

static long baseline_allocate(allocator *self, long count, long *out) {
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    long n = 0;
    for (long i = 0; i < self->size; i++) {
        if (bitmap_get(&self->bm, (size_t)i) == 0) {
            out[n++] = i;
            if (n == count) {
                for (long j = 0; j < n; j++) {
                    bitmap_set(&self->bm, (size_t)out[j], 1);
                }
                return n;
            }
        }
    }
    return 0;
}

static bool baseline_deallocate(allocator *self, const long *indices, long n) {
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
    }
    return true;
}

static long baseline_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double baseline_aux_mem(allocator *self) {
    (void)self;
    return 0.0;
}

static void baseline_destroy(allocator *self) {
    allocator_base_destroy(self);
    free(self);
}

allocator *baseline_create(long size) {
    allocator *self = (allocator *)malloc(sizeof(allocator));
    allocator_base_init(self, size);
    self->allocate = baseline_allocate;
    self->deallocate = baseline_deallocate;
    self->count_free = baseline_count_free;
    self->aux_mem_bytes = baseline_aux_mem;
    self->destroy = baseline_destroy;
    return self;
}
