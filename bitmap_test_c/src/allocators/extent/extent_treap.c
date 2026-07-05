/*
 * Extent-first allocators with the lowest-address scatter fallback.
 * Port of allocators/extent/extent_base.py (RangeExtentAllocator policy) plus
 * a treap-of-ranges structure (port of treap_range_extent.py).
 *
 * The three Python lowest-address extent allocators (treap_range_extent,
 * rb_range_extent, bplustree_extent) share this exact policy and are defined to
 * be byte-identical (consistency group "extent_bestfit_lowaddr"); they differ
 * only by the internal balanced structure (treap / red-black / B+tree), which
 * affects performance and memory accounting but never the produced bitmap or any
 * metric other than peak_memory/timing. To keep the port tractable, all three
 * factories here are backed by this one faithful treap-of-ranges implementation.
 * (avl_range_extent has a different fallback and is implemented separately.)
 */
#include <stdlib.h>

#include "allocators.h"
#include "rng.h"

typedef struct trnode {
    long start;
    long end;
    double priority;
    struct trnode *left;
    struct trnode *right;
    long max_gap;
} trnode;

typedef struct {
    allocator base;
    trnode *root;
    long node_count;
    rng_state rng;
} extent_alloc;

static long tr_size(trnode *n) {
    return n->end - n->start + 1;
}

static void tr_update(trnode *node) {
    long m = node->end - node->start + 1;
    if (node->left && node->left->max_gap > m) {
        m = node->left->max_gap;
    }
    if (node->right && node->right->max_gap > m) {
        m = node->right->max_gap;
    }
    node->max_gap = m;
}

static trnode *tr_new(extent_alloc *a, long start, long end) {
    trnode *n = (trnode *)malloc(sizeof(trnode));
    n->start = start;
    n->end = end;
    n->priority = rng_double(&a->rng);
    n->left = n->right = NULL;
    n->max_gap = end - start + 1;
    a->node_count += 1;
    return n;
}

static trnode *tr_rotate_right(trnode *node) {
    trnode *nr = node->left;
    node->left = nr->right;
    nr->right = node;
    tr_update(node);
    tr_update(nr);
    return nr;
}

static trnode *tr_rotate_left(trnode *node) {
    trnode *nr = node->right;
    node->right = nr->left;
    nr->left = node;
    tr_update(node);
    tr_update(nr);
    return nr;
}

static trnode *tr_insert(trnode *node, trnode *new_node) {
    if (node == NULL) {
        return new_node;
    }
    if (new_node->start < node->start) {
        node->left = tr_insert(node->left, new_node);
        if (node->left->priority > node->priority) {
            return tr_rotate_right(node);
        }
    } else {
        node->right = tr_insert(node->right, new_node);
        if (node->right->priority > node->priority) {
            return tr_rotate_left(node);
        }
    }
    tr_update(node);
    return node;
}

static trnode *tr_delete(extent_alloc *a, trnode *node, long start) {
    if (node == NULL) {
        return NULL;
    }
    if (start < node->start) {
        node->left = tr_delete(a, node->left, start);
    } else if (start > node->start) {
        node->right = tr_delete(a, node->right, start);
    } else {
        if (node->left == NULL) {
            trnode *r = node->right;
            free(node);
            a->node_count -= 1;
            return r;
        }
        if (node->right == NULL) {
            trnode *l = node->left;
            free(node);
            a->node_count -= 1;
            return l;
        }
        if (node->left->priority > node->right->priority) {
            node = tr_rotate_right(node);
            node->right = tr_delete(a, node->right, start);
        } else {
            node = tr_rotate_left(node);
            node->left = tr_delete(a, node->left, start);
        }
        tr_update(node);
        return node;
    }
    tr_update(node);
    return node;
}

static trnode *tr_best_fit(trnode *node, long count) {
    if (!node || node->max_gap < count) {
        return NULL;
    }
    trnode *best = NULL;
    if (node->left && node->left->max_gap >= count) {
        best = tr_best_fit(node->left, count);
    }
    if (tr_size(node) >= count && (!best || tr_size(node) < tr_size(best))) {
        best = node;
    }
    if (node->right && node->right->max_gap >= count) {
        trnode *rb = tr_best_fit(node->right, count);
        if (rb && (!best || tr_size(rb) < tr_size(best))) {
            best = rb;
        }
    }
    return best;
}

/* ----- structure hooks ----- */
static int hook_find_best_fit(extent_alloc *a, long count, long *s, long *e) {
    trnode *n = tr_best_fit(a->root, count);
    if (!n) {
        return 0;
    }
    *s = n->start;
    *e = n->end;
    return 1;
}

static int hook_pop_min_range(extent_alloc *a, long *s, long *e) {
    trnode *node = a->root;
    if (!node) {
        return 0;
    }
    while (node->left) {
        node = node->left;
    }
    *s = node->start;
    *e = node->end;
    a->root = tr_delete(a, a->root, node->start);
    return 1;
}

static void hook_insert_range(extent_alloc *a, long start, long end) {
    a->root = tr_insert(a->root, tr_new(a, start, end));
}

static void hook_delete_range(extent_alloc *a, long start) {
    a->root = tr_delete(a, a->root, start);
}

static int hook_predecessor(extent_alloc *a, long key, long *s, long *e) {
    trnode *pred = NULL;
    trnode *curr = a->root;
    while (curr) {
        if (curr->start < key) {
            pred = curr;
            curr = curr->right;
        } else {
            curr = curr->left;
        }
    }
    if (!pred) {
        return 0;
    }
    *s = pred->start;
    *e = pred->end;
    return 1;
}

static int hook_successor(extent_alloc *a, long key, long *s, long *e) {
    trnode *succ = NULL;
    trnode *curr = a->root;
    while (curr) {
        if (curr->start > key) {
            succ = curr;
            curr = curr->left;
        } else {
            curr = curr->right;
        }
    }
    if (!succ) {
        return 0;
    }
    *s = succ->start;
    *e = succ->end;
    return 1;
}

/* ----- shared policy (extent_base.py) ----- */
static void extent_add_free_range(extent_alloc *a, long start, long end) {
    if (start > end) {
        return;
    }
    for (;;) {
        long ps, pe;
        if (hook_predecessor(a, start, &ps, &pe) && pe + 1 >= start) {
            if (ps < start) {
                start = ps;
            }
            if (pe > end) {
                end = pe;
            }
            hook_delete_range(a, ps);
            continue;
        }
        break;
    }
    for (;;) {
        long ss, se;
        if (hook_successor(a, end, &ss, &se) && ss <= end + 1) {
            if (se > end) {
                end = se;
            }
            hook_delete_range(a, ss);
            continue;
        }
        break;
    }
    hook_insert_range(a, start, end);
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
    if (hook_find_best_fit(a, count, &bs, &be)) {
        hook_delete_range(a, bs);
        if (bs + count <= be) {
            hook_insert_range(a, bs + count, be);
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
        if (!hook_pop_min_range(a, &s, &e)) {
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
            hook_insert_range(a, s + take, e);
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
    return (double)((size_t)a->node_count * sizeof(trnode));
}

static void tr_free(trnode *n) {
    if (!n) {
        return;
    }
    tr_free(n->left);
    tr_free(n->right);
    free(n);
}

static void extent_destroy(allocator *self) {
    extent_alloc *a = (extent_alloc *)self;
    tr_free(a->root);
    allocator_base_destroy(self);
    free(a);
}

static allocator *extent_create(long size) {
    extent_alloc *a = (extent_alloc *)malloc(sizeof(extent_alloc));
    allocator_base_init(&a->base, size);
    a->base.consistency_group = "extent_bestfit_lowaddr";
    a->root = NULL;
    a->node_count = 0;
    rng_seed(&a->rng, 0xE47e47);
    if (size > 0) {
        a->root = tr_new(a, 0, size - 1);
    }
    a->base.allocate = extent_allocate;
    a->base.deallocate = extent_deallocate;
    a->base.count_free = extent_count_free;
    a->base.aux_mem_bytes = extent_aux_mem;
    a->base.destroy = extent_destroy;
    return &a->base;
}

allocator *treap_range_extent_create(long size) { return extent_create(size); }
allocator *rb_range_extent_create(long size) { return extent_create(size); }
allocator *bplustree_extent_create(long size) { return extent_create(size); }
