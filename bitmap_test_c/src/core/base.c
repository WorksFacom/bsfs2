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

bool allocator_validate_deallocation(allocator *self, const long *indices, long n) {
    if (n <= 0) {
        return true; /* empty: nothing to reject */
    }

    /* O(n) like Python's set(): tag each index's byte with 2, a second hit is a duplicate. a qsort
     * here cost more than most allocators' own dealloc work. bytes are restored before returning. */
    uint8_t *data = self->bm.data;
    long i = 0;
    bool ok = true;
    for (; i < n; i++) {
        long idx = indices[i];
        if (idx < 0 || idx >= self->size || data[idx] != 1) {
            ok = false;
            break;
        }
        data[idx] = 2;
    }
    for (long j = 0; j < i; j++) {
        data[indices[j]] = 1;
    }
    return ok;
}

void allocator_rollback(allocator *self, const long *allocated, long n) {
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)allocated[i], 0);
    }
}
