/*
 * shared extent-first policy, port of allocators/extent/extent_base.py::RangeExtentAllocator.
 *
 * the policy (best-fit, lowest-address scatter fallback, coalescing frees) lives in extent_base.c
 * and only talks to the free-range structure through these hooks, so treap, red-black and B+tree
 * extents differ in structure alone. ranges are inclusive [start, end].
 */
#ifndef EXTENT_H
#define EXTENT_H

#include "base.h"

typedef struct {
    /* smallest range with size >= count, not removed. 0 if none */
    int (*find_best_fit)(void *st, long count, long *start, long *end);
    /* remove and return the lowest-start range. 0 if empty */
    int (*pop_min_range)(void *st, long *start, long *end);
    void (*insert_range)(void *st, long start, long end);
    void (*delete_range)(void *st, long start);
    /* greatest start < key / smallest start > key. 0 if none */
    int (*predecessor)(void *st, long key, long *start, long *end);
    int (*successor)(void *st, long key, long *start, long *end);
    double (*aux_mem_bytes)(void *st);
    void (*destroy)(void *st);
} extent_ops;

/* takes ownership of `st`, which must already hold the single range [0, size-1] */
allocator *extent_policy_create(long size, const extent_ops *ops, void *st);

#endif /* EXTENT_H */
