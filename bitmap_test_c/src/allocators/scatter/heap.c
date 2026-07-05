/*
 * Lazy allocator using a min-heap for recycled blocks + next_fresh cursor.
 * Port of allocators/scatter/heap.py.
 */
#include <stdlib.h>

#include "allocators.h"

typedef struct {
    allocator base;
    long *heap;
    long heap_len;
    long heap_cap;
    long next_fresh;
} heap_alloc;

static void heap_push(heap_alloc *a, long val) {
    if (a->heap_len == a->heap_cap) {
        a->heap_cap = a->heap_cap ? a->heap_cap * 2 : 16;
        a->heap = (long *)realloc(a->heap, (size_t)a->heap_cap * sizeof(long));
    }
    long i = a->heap_len++;
    a->heap[i] = val;
    while (i > 0) {
        long parent = (i - 1) / 2;
        if (a->heap[parent] <= a->heap[i]) {
            break;
        }
        long t = a->heap[parent];
        a->heap[parent] = a->heap[i];
        a->heap[i] = t;
        i = parent;
    }
}

static long heap_pop(heap_alloc *a) {
    long top = a->heap[0];
    a->heap_len--;
    if (a->heap_len > 0) {
        a->heap[0] = a->heap[a->heap_len];
        long i = 0;
        for (;;) {
            long l = 2 * i + 1, r = 2 * i + 2, smallest = i;
            if (l < a->heap_len && a->heap[l] < a->heap[smallest]) {
                smallest = l;
            }
            if (r < a->heap_len && a->heap[r] < a->heap[smallest]) {
                smallest = r;
            }
            if (smallest == i) {
                break;
            }
            long t = a->heap[i];
            a->heap[i] = a->heap[smallest];
            a->heap[smallest] = t;
            i = smallest;
        }
    }
    return top;
}

static long heap_pop_min_free(heap_alloc *a) {
    long size = a->base.size;
    if (a->next_fresh >= size && a->heap_len == 0) {
        return -1;
    }
    if (a->heap_len == 0) {
        return a->next_fresh++;
    }
    if (a->next_fresh >= size || a->heap[0] < a->next_fresh) {
        return heap_pop(a);
    }
    return a->next_fresh++;
}

static long heap_allocate(allocator *self, long count, long *out) {
    heap_alloc *a = (heap_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long idx = heap_pop_min_free(a);
        bitmap_set(&self->bm, (size_t)idx, 1);
        out[i] = idx;
    }
    return count;
}

static bool heap_deallocate(allocator *self, const long *indices, long n) {
    heap_alloc *a = (heap_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        heap_push(a, indices[i]);
    }
    return true;
}

static long heap_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double heap_aux_mem(allocator *self) {
    heap_alloc *a = (heap_alloc *)self;
    return (double)((size_t)a->heap_len * sizeof(long));
}

static void heap_destroy(allocator *self) {
    heap_alloc *a = (heap_alloc *)self;
    free(a->heap);
    allocator_base_destroy(self);
    free(a);
}

allocator *heap_create(long size) {
    heap_alloc *a = (heap_alloc *)malloc(sizeof(heap_alloc));
    allocator_base_init(&a->base, size);
    a->heap = NULL;
    a->heap_len = 0;
    a->heap_cap = 0;
    a->next_fresh = 0;
    a->base.allocate = heap_allocate;
    a->base.deallocate = heap_deallocate;
    a->base.count_free = heap_count_free;
    a->base.aux_mem_bytes = heap_aux_mem;
    a->base.destroy = heap_destroy;
    return &a->base;
}
