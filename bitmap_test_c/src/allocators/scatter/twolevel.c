/*
 * two-level index allocator, buckets + one forward scan. port of allocators/scatter/two_level_index.py.
 */
#include <stdlib.h>

#include "allocators.h"

#define BUCKET_SIZE 100

typedef struct {
    allocator base;
    long num_buckets;
    long *bucket_free_count;
    long last_bucket_idx;
} twolevel_alloc;

static long tl_bucket_idx(long block_idx) {
    return block_idx / BUCKET_SIZE;
}

static long tl_bucket_start(long bucket_idx) {
    return bucket_idx * BUCKET_SIZE;
}

static long tl_bucket_end(twolevel_alloc *a, long bucket_idx) {
    long e = (bucket_idx + 1) * BUCKET_SIZE;
    return e < a->base.size ? e : a->base.size;
}

static long twolevel_allocate(allocator *self, long count, long *out) {
    twolevel_alloc *a = (twolevel_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }

    long n = 0;
    long *bfc = a->bucket_free_count;
    long bucket_idx = a->last_bucket_idx;

    while (bucket_idx < a->num_buckets && bfc[bucket_idx] == 0) {
        bucket_idx += 1;
    }

    if (bucket_idx == a->num_buckets) {
        bucket_idx = 0;
        while (bucket_idx < a->last_bucket_idx && bfc[bucket_idx] == 0) {
            bucket_idx += 1;
        }
        if (bucket_idx == a->last_bucket_idx) {
            return 0;
        }
    }

    long pos = tl_bucket_start(bucket_idx);
    long bucket_end = tl_bucket_end(a, bucket_idx);

    while (n < count) {
        if (pos >= bucket_end) {
            bucket_idx += 1;
            while (bucket_idx < a->num_buckets && bfc[bucket_idx] == 0) {
                bucket_idx += 1;
            }
            if (bucket_idx >= a->num_buckets) {
                break;
            }
            pos = tl_bucket_start(bucket_idx);
            bucket_end = tl_bucket_end(a, bucket_idx);
        }

        if (bitmap_get(&self->bm, (size_t)pos) == 0) {
            out[n++] = pos;
            bitmap_set(&self->bm, (size_t)pos, 1);
            bfc[bucket_idx] -= 1;
        }
        pos += 1;
    }

    if (n < count) {
        for (long i = 0; i < n; i++) {
            bitmap_set(&self->bm, (size_t)out[i], 0);
            bfc[tl_bucket_idx(out[i])] += 1;
        }
        return 0;
    }

    a->last_bucket_idx = bucket_idx;
    return n;
}

static bool twolevel_deallocate(allocator *self, const long *indices, long n) {
    twolevel_alloc *a = (twolevel_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        a->bucket_free_count[tl_bucket_idx(indices[i])] += 1;
    }
    a->last_bucket_idx = 0;
    return true;
}

static long twolevel_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double twolevel_aux_mem(allocator *self) {
    twolevel_alloc *a = (twolevel_alloc *)self;
    return (double)((size_t)a->num_buckets * sizeof(long));
}

static void twolevel_destroy(allocator *self) {
    twolevel_alloc *a = (twolevel_alloc *)self;
    free(a->bucket_free_count);
    allocator_base_destroy(self);
    free(a);
}

allocator *twolevel_create(long size) {
    twolevel_alloc *a = (twolevel_alloc *)malloc(sizeof(twolevel_alloc));
    allocator_base_init(&a->base, size);
    a->num_buckets = (size + BUCKET_SIZE - 1) / BUCKET_SIZE;
    a->bucket_free_count = (long *)malloc((size_t)(a->num_buckets ? a->num_buckets : 1) * sizeof(long));
    for (long i = 0; i < a->num_buckets; i++) {
        a->bucket_free_count[i] = BUCKET_SIZE;
    }
    long last_bucket_size = size % BUCKET_SIZE;
    if (last_bucket_size > 0) {
        a->bucket_free_count[a->num_buckets - 1] = last_bucket_size;
    }
    a->last_bucket_idx = 0;
    a->base.allocate = twolevel_allocate;
    a->base.deallocate = twolevel_deallocate;
    a->base.count_free = twolevel_count_free;
    a->base.aux_mem_bytes = twolevel_aux_mem;
    a->base.destroy = twolevel_destroy;
    return &a->base;
}
