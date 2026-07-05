/*
 * Sparse implicit segment tree of recycled blocks + next_fresh cursor.
 * Port of allocators/scatter/segment_tree.py.
 *
 * `recycled` maps node-index -> recycled-free count in that interval.
 */
#include <stdlib.h>

#include "allocators.h"
#include "longmap.h"

typedef struct {
    allocator base;
    longmap recycled;
    long next_fresh;
} segtree_alloc;

static long sg_node_recycled(segtree_alloc *a, long node) {
    return lm_get(&a->recycled, node, 0);
}

static void sg_insert_recycled(segtree_alloc *a, long index) {
    long size = a->base.size;
    /* path buffer: depth <= log2(size)+2 */
    long path[64];
    long depth = 0;
    long node = 1;
    long left = 0;
    long right = size - 1;

    while (left != right) {
        path[depth++] = node;
        long mid = (left + right) / 2;
        if (index <= mid) {
            node = node * 2;
            right = mid;
        } else {
            node = node * 2 + 1;
            left = mid + 1;
        }
    }

    lm_set(&a->recycled, node, 1);

    for (long i = depth - 1; i >= 0; i--) {
        long parent = path[i];
        long lc = parent * 2;
        long rc = lc + 1;
        long total = lm_get(&a->recycled, lc, 0) + lm_get(&a->recycled, rc, 0);
        if (total) {
            lm_set(&a->recycled, parent, total);
        } else {
            lm_del(&a->recycled, parent);
        }
    }
}

static long sg_find_min_recycled_idx(segtree_alloc *a) {
    if (sg_node_recycled(a, 1) == 0) {
        return -1;
    }
    long node = 1;
    long left = 0;
    long right = a->base.size - 1;
    while (left != right) {
        long mid = (left + right) / 2;
        long left_node = node * 2;
        if (lm_get(&a->recycled, left_node, 0) > 0) {
            node = left_node;
            right = mid;
        } else {
            node = left_node + 1;
            left = mid + 1;
        }
    }
    return left;
}

static long sg_pop_min_recycled(segtree_alloc *a) {
    if (sg_node_recycled(a, 1) == 0) {
        return -1;
    }
    long path[64];
    long depth = 0;
    long node = 1;
    long left = 0;
    long right = a->base.size - 1;
    while (left != right) {
        path[depth++] = node;
        long mid = (left + right) / 2;
        long left_node = node * 2;
        if (lm_get(&a->recycled, left_node, 0) > 0) {
            node = left_node;
            right = mid;
        } else {
            node = left_node + 1;
            left = mid + 1;
        }
    }
    long result = left;
    lm_del(&a->recycled, node);

    for (long i = depth - 1; i >= 0; i--) {
        long parent = path[i];
        long lc = parent * 2;
        long rc = lc + 1;
        long total = lm_get(&a->recycled, lc, 0) + lm_get(&a->recycled, rc, 0);
        if (total) {
            lm_set(&a->recycled, parent, total);
        } else {
            lm_del(&a->recycled, parent);
        }
    }
    return result;
}

static void sg_rollback_allocated(segtree_alloc *a, const long *allocated, long n) {
    allocator_rollback(&a->base, allocated, n);
    for (long i = 0; i < n; i++) {
        sg_insert_recycled(a, allocated[i]);
    }
}

static long segtree_allocate(allocator *self, long count, long *out) {
    segtree_alloc *a = (segtree_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }

    if (a->recycled.count == 0) {
        if (a->next_fresh + count > self->size) {
            return 0;
        }
        long start = a->next_fresh;
        long end = start + count;
        bitmap_set_range(&self->bm, (size_t)start, (size_t)end, 1);
        a->next_fresh = end;
        for (long i = 0; i < count; i++) {
            out[i] = start + i;
        }
        return count;
    }

    long n = 0;
    long next_fresh = a->next_fresh;
    long size = self->size;

    for (long k = 0; k < count; k++) {
        int recycled_has_blocks = a->recycled.count != 0;
        long fresh_idx = (next_fresh < size) ? next_fresh : -1;

        if (!recycled_has_blocks && fresh_idx == -1) {
            sg_rollback_allocated(a, out, n);
            return 0;
        }

        long idx;
        if (!recycled_has_blocks) {
            idx = fresh_idx;
            next_fresh += 1;
        } else if (fresh_idx == -1) {
            idx = sg_pop_min_recycled(a);
        } else {
            long recycled_idx = sg_find_min_recycled_idx(a);
            if (fresh_idx <= recycled_idx) {
                idx = fresh_idx;
                next_fresh += 1;
            } else {
                idx = sg_pop_min_recycled(a);
            }
        }

        bitmap_set(&self->bm, (size_t)idx, 1);
        out[n++] = idx;
    }

    a->next_fresh = next_fresh;
    return n;
}

static int cmp_long_asc(const void *x, const void *y) {
    long a = *(const long *)x, b = *(const long *)y;
    return (a > b) - (a < b);
}

static bool segtree_deallocate(allocator *self, const long *indices, long n) {
    segtree_alloc *a = (segtree_alloc *)self;
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
    for (long i = 0; i < n; i++) {
        sg_insert_recycled(a, sorted[i]);
    }
    free(sorted);
    return true;
}

static long segtree_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double segtree_aux_mem(allocator *self) {
    segtree_alloc *a = (segtree_alloc *)self;
    return (double)((size_t)a->recycled.count * sizeof(lm_entry));
}

static void segtree_destroy(allocator *self) {
    segtree_alloc *a = (segtree_alloc *)self;
    lm_free(&a->recycled);
    allocator_base_destroy(self);
    free(a);
}

allocator *segtree_create(long size) {
    segtree_alloc *a = (segtree_alloc *)malloc(sizeof(segtree_alloc));
    allocator_base_init(&a->base, size);
    lm_init(&a->recycled);
    a->next_fresh = 0;
    a->base.allocate = segtree_allocate;
    a->base.deallocate = segtree_deallocate;
    a->base.count_free = segtree_count_free;
    a->base.aux_mem_bytes = segtree_aux_mem;
    a->base.destroy = segtree_destroy;
    return &a->base;
}
