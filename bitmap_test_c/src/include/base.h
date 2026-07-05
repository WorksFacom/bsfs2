/*
 * BaseAllocator contract. Port of core/base_allocator.py::BaseAllocator.
 *
 * Every concrete allocator embeds `allocator base;` as its first member, so a
 * pointer to the concrete struct is also a valid `allocator *`. The vtable
 * function pointers implement the abstract contract; the helper functions
 * (allocator_validate_deallocation / allocator_rollback) are the shared
 * concrete logic used by subclasses.
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

    /* allocate_blocks(count): fills `out` (capacity >= count) with the allocated
     * block indices and returns how many were written. Returns 0 for the Python
     * "empty list" result (count<=0, not enough free, or failure). */
    long (*allocate)(allocator *self, long count, long *out);

    /* deallocate_blocks(indices): returns true on success, false on validation
     * failure (mirrors the Python bool return). */
    bool (*deallocate)(allocator *self, const long *indices, long n);

    long (*count_free)(allocator *self);

    /* Auxiliary (non-bitmap) memory in bytes; replaces Python sys.getsizeof. */
    double (*aux_mem_bytes)(allocator *self);

    void (*destroy)(allocator *self);
};

/* Initialize the embedded bitmap and defaults; consistency_group defaults to
 * "scatter" and may be overridden by the concrete allocator after this call. */
void allocator_base_init(allocator *self, long size);
void allocator_base_destroy(allocator *self);

long allocator_free_remaining(const allocator *self);

/* validate_deallocation: false on duplicates, out-of-bounds, or double-free. */
bool allocator_validate_deallocation(allocator *self, const long *indices, long n);

/* rollback_bitmap_allocation: clear each allocated index back to 0. */
void allocator_rollback(allocator *self, const long *allocated, long n);

#endif /* BASE_H */
