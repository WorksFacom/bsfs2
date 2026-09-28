/*
 * BaseAllocator contract, port of core/base_allocator.py::BaseAllocator.
 *
 * every allocator embeds `allocator base;` as its first member, so a pointer to the concrete struct
 * is also a valid `allocator *`. the vtable is the abstract contract; allocator_validate_deallocation
 * and allocator_rollback are the shared helpers.
 */
#ifndef BASE_H
#define BASE_H

#include <stdbool.h>
#include <stddef.h>

#include "bitmap.h"

typedef struct allocator allocator;

struct allocator {
    bitmap bm;
    long size;
    const char *consistency_group;

    /* fills `out` (capacity >= count) and returns how many were written. 0 is Python's empty list:
     * count <= 0, not enough free, or failure. */
    long (*allocate)(allocator *self, long count, long *out);

    /* false on validation failure, like the Python bool return */
    bool (*deallocate)(allocator *self, const long *indices, long n);

    long (*count_free)(allocator *self);

    /* aux (non-bitmap) memory in bytes, stands in for Python's sys.getsizeof */
    double (*aux_mem_bytes)(allocator *self);

    void (*destroy)(allocator *self);
};

/* consistency_group defaults to "scatter", allocators override it after this call */
void allocator_base_init(allocator *self, long size);
void allocator_base_destroy(allocator *self);

long allocator_free_remaining(const allocator *self);

/* validate_deallocation: false on duplicates, out-of-bounds, or double-free. */
bool allocator_validate_deallocation(allocator *self, const long *indices, long n);

/* rollback_bitmap_allocation: clear each allocated index back to 0 */
void allocator_rollback(allocator *self, const long *allocated, long n);

#endif /* BASE_H */
