/*
 * AVL tree of free ranges, extent-first with a *largest-range* fallback. port of
 * allocators/extent/avl_range_extent.py. own consistency group, "extent_bestfit_largest".
 *
 * the Python `changed` short-circuit is only a speedup. recomputing height/max_gap and rebalancing
 * all the way to the root gives the same tree, so it's left out.
 */
#include <stdlib.h>

#include "allocators.h"

typedef struct arnode {
    long start;
    long end;
    struct arnode *left;
    struct arnode *right;
    int height;
    long max_gap;
} arnode;

typedef struct {
    allocator base;
    arnode *root;
    long node_count;
} are_alloc;

static int ar_h(arnode *n) {
    return n ? n->height : 0;
}

static long ar_size(arnode *n) {
    return n->end - n->start + 1;
}

static void ar_update(arnode *node) {
    int lh = ar_h(node->left);
    int rh = ar_h(node->right);
    node->height = (lh > rh ? lh : rh) + 1;
    long m = node->end - node->start + 1;
    if (node->left && node->left->max_gap > m) {
        m = node->left->max_gap;
    }
    if (node->right && node->right->max_gap > m) {
        m = node->right->max_gap;
    }
    node->max_gap = m;
}

static int ar_balance(arnode *n) {
    return n ? ar_h(n->left) - ar_h(n->right) : 0;
}

static arnode *ar_new(are_alloc *a, long start, long end) {
    arnode *n = (arnode *)malloc(sizeof(arnode));
    n->start = start;
    n->end = end;
    n->left = n->right = NULL;
    n->height = 1;
    n->max_gap = end - start + 1;
    a->node_count += 1;
    return n;
}

static arnode *ar_rotate_left(arnode *node) {
    arnode *rc = node->right;
    node->right = rc->left;
    rc->left = node;
    ar_update(node);
    ar_update(rc);
    return rc;
}

static arnode *ar_rotate_right(arnode *node) {
    arnode *lc = node->left;
    node->left = lc->right;
    lc->right = node;
    ar_update(node);
    ar_update(lc);
    return lc;
}

static arnode *ar_insert(are_alloc *a, arnode *node, long start, long end) {
    if (!node) {
        return ar_new(a, start, end);
    }
    if (start < node->start) {
        node->left = ar_insert(a, node->left, start, end);
    } else {
        node->right = ar_insert(a, node->right, start, end);
    }
    ar_update(node);
    int balance = ar_balance(node);

    if (balance > 1 && start < node->left->start) {
        return ar_rotate_right(node);
    }
    if (balance < -1 && start > node->right->start) {
        return ar_rotate_left(node);
    }
    if (balance > 1 && start > node->left->start) {
        node->left = ar_rotate_left(node->left);
        return ar_rotate_right(node);
    }
    if (balance < -1 && start < node->right->start) {
        node->right = ar_rotate_right(node->right);
        return ar_rotate_left(node);
    }
    return node;
}

static arnode *ar_find_min(arnode *node) {
    while (node->left) {
        node = node->left;
    }
    return node;
}

static arnode *ar_delete(are_alloc *a, arnode *node, long start) {
    if (!node) {
        return NULL;
    }
    if (start < node->start) {
        node->left = ar_delete(a, node->left, start);
    } else if (start > node->start) {
        node->right = ar_delete(a, node->right, start);
    } else {
        if (!node->left) {
            arnode *r = node->right;
            free(node);
            a->node_count -= 1;
            return r;
        }
        if (!node->right) {
            arnode *l = node->left;
            free(node);
            a->node_count -= 1;
            return l;
        }
        arnode *min_node = ar_find_min(node->right);
        node->start = min_node->start;
        node->end = min_node->end;
        node->right = ar_delete(a, node->right, min_node->start);
    }
    ar_update(node);
    int balance = ar_balance(node);

    if (balance > 1 && ar_balance(node->left) >= 0) {
        return ar_rotate_right(node);
    }
    if (balance > 1 && ar_balance(node->left) < 0) {
        node->left = ar_rotate_left(node->left);
        return ar_rotate_right(node);
    }
    if (balance < -1 && ar_balance(node->right) <= 0) {
        return ar_rotate_left(node);
    }
    if (balance < -1 && ar_balance(node->right) > 0) {
        node->right = ar_rotate_right(node->right);
        return ar_rotate_left(node);
    }
    return node;
}

static arnode *ar_best_fit(arnode *node, long count) {
    if (!node || node->max_gap < count) {
        return NULL;
    }
    arnode *best = NULL;
    if (node->left && node->left->max_gap >= count) {
        best = ar_best_fit(node->left, count);
    }
    if (ar_size(node) >= count && (!best || ar_size(node) < ar_size(best))) {
        best = node;
    }
    if (node->right && node->right->max_gap >= count) {
        arnode *rb = ar_best_fit(node->right, count);
        if (rb && (!best || ar_size(rb) < ar_size(best))) {
            best = rb;
        }
    }
    return best;
}

static arnode *ar_find_largest(arnode *node) {
    if (!node) {
        return NULL;
    }
    if (node->left && node->left->max_gap == node->max_gap) {
        return ar_find_largest(node->left);
    }
    if (node->right && node->right->max_gap == node->max_gap) {
        return ar_find_largest(node->right);
    }
    return node;
}

static arnode *ar_find_predecessor(arnode *node, long key) {
    arnode *pred = NULL;
    arnode *curr = node;
    while (curr) {
        if (curr->start < key) {
            pred = curr;
            curr = curr->right;
        } else {
            curr = curr->left;
        }
    }
    return pred;
}

static arnode *ar_find_successor(arnode *node, long key) {
    arnode *succ = NULL;
    arnode *curr = node;
    while (curr) {
        if (curr->start > key) {
            succ = curr;
            curr = curr->left;
        } else {
            curr = curr->right;
        }
    }
    return succ;
}

static void ar_add_free_range(are_alloc *a, long start, long end) {
    if (start > end) {
        return;
    }
    for (;;) {
        arnode *pred = ar_find_predecessor(a->root, start);
        if (pred && pred->end + 1 >= start) {
            if (pred->start < start) {
                start = pred->start;
            }
            if (pred->end > end) {
                end = pred->end;
            }
            a->root = ar_delete(a, a->root, pred->start);
            continue;
        }
        break;
    }
    for (;;) {
        arnode *succ = ar_find_successor(a->root, end);
        if (succ && succ->start <= end + 1) {
            if (succ->end > end) {
                end = succ->end;
            }
            a->root = ar_delete(a, a->root, succ->start);
            continue;
        }
        break;
    }
    a->root = ar_insert(a, a->root, start, end);
}

static long are_allocate(allocator *self, long count, long *out) {
    are_alloc *a = (are_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }

    arnode *node = ar_best_fit(a->root, count);
    if (node) {
        long rs = node->start, re = node->end;
        a->root = ar_delete(a, a->root, rs);
        for (long i = 0; i < count; i++) {
            out[i] = rs + i;
        }
        if (rs + count <= re) {
            a->root = ar_insert(a, a->root, rs + count, re);
        }
        for (long i = 0; i < count; i++) {
            bitmap_set(&self->bm, (size_t)out[i], 1);
        }
        return count;
    }

    long n = 0;
    long remaining = count;
    while (remaining > 0) {
        node = ar_find_largest(a->root);
        if (!node) {
            allocator_rollback(self, out, n);
            for (long i = 0; i < n; i++) {
                ar_add_free_range(a, out[i], out[i]);
            }
            return 0;
        }
        long rs = node->start, re = node->end;
        a->root = ar_delete(a, a->root, rs);
        long take = remaining < (re - rs + 1) ? remaining : (re - rs + 1);
        for (long k = 0; k < take; k++) {
            out[n++] = rs + k;
        }
        if (rs + take <= re) {
            a->root = ar_insert(a, a->root, rs + take, re);
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

static bool are_deallocate(allocator *self, const long *indices, long n) {
    are_alloc *a = (are_alloc *)self;
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
        ar_add_free_range(a, range_start, range_end);
        range_start = sorted[i];
        range_end = sorted[i];
    }
    ar_add_free_range(a, range_start, range_end);
    free(sorted);
    return true;
}

static long are_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double are_aux_mem(allocator *self) {
    are_alloc *a = (are_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(arnode));
}

static void ar_free(arnode *n) {
    if (!n) {
        return;
    }
    ar_free(n->left);
    ar_free(n->right);
    free(n);
}

static void are_destroy(allocator *self) {
    are_alloc *a = (are_alloc *)self;
    ar_free(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *avl_range_extent_create(long size) {
    are_alloc *a = (are_alloc *)malloc(sizeof(are_alloc));
    allocator_base_init(&a->base, size);
    a->base.consistency_group = "extent_bestfit_largest";
    a->root = NULL;
    a->node_count = 0;
    if (size > 0) {
        a->root = ar_new(a, 0, size - 1);
    }
    a->base.allocate = are_allocate;
    a->base.deallocate = are_deallocate;
    a->base.count_free = are_count_free;
    a->base.aux_mem_bytes = are_aux_mem;
    a->base.destroy = are_destroy;
    return &a->base;
}
