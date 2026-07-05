#include "base.h"

#include <stdlib.h>

void allocator_base_init(allocator *self, long size) {
    bitmap_init(&self->bm, (size_t)size);
    self->size = size;
    self->consistency_group = "scatter";
    self->allocate = NULL;
    self->deallocate = NULL;
    self->count_free = NULL;
    self->aux_mem_bytes = NULL;
    self->destroy = NULL;
}

void allocator_base_destroy(allocator *self) {
    bitmap_destroy(&self->bm);
}

long allocator_free_remaining(const allocator *self) {
    return (long)self->bm.free_count;
}

static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a;
    long y = *(const long *)b;
    return (x > y) - (x < y);
}

bool allocator_validate_deallocation(allocator *self, const long *indices, long n) {
    if (n <= 0) {
        return true; /* empty set: len(set)==len==0, loop body never rejects */
    }

    /* duplicate check: len(set(indices)) != len(indices) */
    long *tmp = (long *)malloc((size_t)n * sizeof(long));
    for (long i = 0; i < n; i++) {
        tmp[i] = indices[i];
    }
    qsort(tmp, (size_t)n, sizeof(long), cmp_long);
    for (long i = 1; i < n; i++) {
        if (tmp[i] == tmp[i - 1]) {
            free(tmp);
            return false;
        }
    }
    free(tmp);

    for (long i = 0; i < n; i++) {
        long idx = indices[i];
        if (idx < 0 || idx >= self->size) {
            return false;
        }
        if (bitmap_get(&self->bm, (size_t)idx) == 0) {
            return false;
        }
    }
    return true;
}

void allocator_rollback(allocator *self, const long *allocated, long n) {
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)allocated[i], 0);
    }
}
