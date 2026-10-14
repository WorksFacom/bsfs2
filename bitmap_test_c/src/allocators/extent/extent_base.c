/*
 * extent-first policy with the lowest-address scatter fallback, port of extent_base.py.
 *
 * treap_range_extent, rb_range_extent and bplustree_extent all run through here and must be
 * byte-identical (group "extent_bestfit_lowaddr"); each brings its own free-range structure via
 * extent_ops. avl_range_extent falls back differently and lives in its own file.
 */
#include <stdlib.h>

#include "allocators.h"
#include "extent.h"

typedef struct {
    allocator base;
    const extent_ops *ops;
    void *st;
} extent_alloc;

static void extent_add_free_range(extent_alloc *a, long start, long end) {
    if (start > end) {
        return;
    }
    for (;;) {
        long ps, pe;
        if (a->ops->predecessor(a->st, start, &ps, &pe) && pe + 1 >= start) {
            if (ps < start) {
                start = ps;
            }
            if (pe > end) {
                end = pe;
            }
            a->ops->delete_range(a->st, ps);
            continue;
        }
        break;
    }
    for (;;) {
        long ss, se;
        if (a->ops->successor(a->st, end, &ss, &se) && ss <= end + 1) {
            if (se > end) {
                end = se;
            }
            a->ops->delete_range(a->st, ss);
            continue;
        }
        break;
    }
    a->ops->insert_range(a->st, start, end);
}

static long extent_allocate(allocator *self, long count, long *out) {
    extent_alloc *a = (extent_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }

    long bs, be;
    if (a->ops->find_best_fit(a->st, count, &bs, &be)) {
        a->ops->delete_range(a->st, bs);
        if (bs + count <= be) {
            a->ops->insert_range(a->st, bs + count, be);
        }
        for (long i = 0; i < count; i++) {
            out[i] = bs + i;
            bitmap_set(&self->bm, (size_t)(bs + i), 1);
        }
        return count;
    }

    long n = 0;
    long remaining = count;
    while (remaining > 0) {
        long s, e;
        if (!a->ops->pop_min_range(a->st, &s, &e)) {
            allocator_rollback(self, out, n);
            for (long i = 0; i < n; i++) {
                extent_add_free_range(a, out[i], out[i]);
            }
            return 0;
        }
        long take = remaining < (e - s + 1) ? remaining : (e - s + 1);
        for (long k = 0; k < take; k++) {
            out[n++] = s + k;
        }
        if (s + take <= e) {
            a->ops->insert_range(a->st, s + take, e);
        }
        remaining -= take;
    }
    for (long i = 0; i < count; i++) {
        bitmap_set(&self->bm, (size_t)out[i], 1);
    }
    return count;
}

static int cmp_long_asc(const void *x, const void *y) {
    long a = *(const long *)x, b = *(const long *)y;
    return (a > b) - (a < b);
}

static bool extent_deallocate(allocator *self, const long *indices, long n) {
    extent_alloc *a = (extent_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
    }
    long *sorted = (long *)malloc((size_t)n * sizeof(long));
    for (long i = 0; i < n; i++) {
        sorted[i] = indices[i];
    }
    qsort(sorted, (size_t)n, sizeof(long), cmp_long_asc);

    long range_start = sorted[0];
    long range_end = range_start;
    for (long i = 1; i < n; i++) {
        if (sorted[i] == range_end + 1) {
            range_end = sorted[i];
            continue;
        }
        extent_add_free_range(a, range_start, range_end);
        range_start = sorted[i];
        range_end = sorted[i];
    }
    extent_add_free_range(a, range_start, range_end);
    free(sorted);
    return true;
}

static long extent_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double extent_aux_mem(allocator *self) {
    extent_alloc *a = (extent_alloc *)self;
    return a->ops->aux_mem_bytes(a->st);
}

static void extent_destroy(allocator *self) {
    extent_alloc *a = (extent_alloc *)self;
    a->ops->destroy(a->st);
    allocator_base_destroy(self);
    free(a);
}

allocator *extent_policy_create(long size, const extent_ops *ops, void *st) {
    extent_alloc *a = (extent_alloc *)malloc(sizeof(extent_alloc));
    allocator_base_init(&a->base, size);
    a->base.consistency_group = "extent_bestfit_lowaddr";
    a->ops = ops;
    a->st = st;
    a->base.allocate = extent_allocate;
    a->base.deallocate = extent_deallocate;
    a->base.count_free = extent_count_free;
    a->base.aux_mem_bytes = extent_aux_mem;
    a->base.destroy = extent_destroy;
    return &a->base;
}
