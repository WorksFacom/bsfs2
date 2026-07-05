/*
 * AVL tree of free ranges [start, end], drained block-by-block (scatter).
 * Port of allocators/scatter/avl_range.py. Bitmap is baseline-identical.
 */
#include <stdlib.h>

#include "allocators.h"

typedef struct avlr_node {
    long start;
    long end;
    struct avlr_node *left;
    struct avlr_node *right;
    int height;
} avlr_node;

typedef struct {
    allocator base;
    avlr_node *root;
    long node_count;
} avlr_alloc;

static int avlr_h(avlr_node *n) {
    return n ? n->height : 0;
}

static int avlr_imax(int a, int b) {
    return a > b ? a : b;
}

static int avlr_balance(avlr_node *n) {
    return n ? avlr_h(n->left) - avlr_h(n->right) : 0;
}

static avlr_node *avlr_new(avlr_alloc *a, long start, long end) {
    avlr_node *n = (avlr_node *)malloc(sizeof(avlr_node));
    n->start = start;
    n->end = end;
    n->left = n->right = NULL;
    n->height = 1;
    a->node_count += 1;
    return n;
}

static avlr_node *avlr_rotate_left(avlr_node *node) {
    avlr_node *rc = node->right;
    node->right = rc->left;
    rc->left = node;
    node->height = 1 + avlr_imax(avlr_h(node->left), avlr_h(node->right));
    rc->height = 1 + avlr_imax(avlr_h(rc->left), avlr_h(rc->right));
    return rc;
}

static avlr_node *avlr_rotate_right(avlr_node *node) {
    avlr_node *lc = node->left;
    node->left = lc->right;
    lc->right = node;
    node->height = 1 + avlr_imax(avlr_h(node->left), avlr_h(node->right));
    lc->height = 1 + avlr_imax(avlr_h(lc->left), avlr_h(lc->right));
    return lc;
}

static avlr_node *avlr_insert(avlr_alloc *a, avlr_node *node, long start, long end) {
    if (!node) {
        return avlr_new(a, start, end);
    }
    if (start < node->start) {
        node->left = avlr_insert(a, node->left, start, end);
    } else {
        node->right = avlr_insert(a, node->right, start, end);
    }
    node->height = 1 + avlr_imax(avlr_h(node->left), avlr_h(node->right));
    int balance = avlr_balance(node);
    if (balance > 1 && start < node->left->start) {
        return avlr_rotate_right(node);
    }
    if (balance < -1 && start > node->right->start) {
        return avlr_rotate_left(node);
    }
    if (balance > 1 && start > node->left->start) {
        node->left = avlr_rotate_left(node->left);
        return avlr_rotate_right(node);
    }
    if (balance < -1 && start < node->right->start) {
        node->right = avlr_rotate_right(node->right);
        return avlr_rotate_left(node);
    }
    return node;
}

static avlr_node *avlr_find_min(avlr_node *node) {
    while (node->left) {
        node = node->left;
    }
    return node;
}

static avlr_node *avlr_delete(avlr_alloc *a, avlr_node *node, long start) {
    if (!node) {
        return NULL;
    }
    if (start < node->start) {
        node->left = avlr_delete(a, node->left, start);
    } else if (start > node->start) {
        node->right = avlr_delete(a, node->right, start);
    } else {
        if (!node->left) {
            avlr_node *r = node->right;
            free(node);
            a->node_count -= 1;
            return r;
        }
        if (!node->right) {
            avlr_node *l = node->left;
            free(node);
            a->node_count -= 1;
            return l;
        }
        avlr_node *min_node = avlr_find_min(node->right);
        node->start = min_node->start;
        node->end = min_node->end;
        node->right = avlr_delete(a, node->right, min_node->start);
    }
    node->height = 1 + avlr_imax(avlr_h(node->left), avlr_h(node->right));
    int balance = avlr_balance(node);
    if (balance > 1 && avlr_balance(node->left) >= 0) {
        return avlr_rotate_right(node);
    }
    if (balance > 1 && avlr_balance(node->left) < 0) {
        node->left = avlr_rotate_left(node->left);
        return avlr_rotate_right(node);
    }
    if (balance < -1 && avlr_balance(node->right) <= 0) {
        return avlr_rotate_left(node);
    }
    if (balance < -1 && avlr_balance(node->right) > 0) {
        node->right = avlr_rotate_right(node->right);
        return avlr_rotate_left(node);
    }
    return node;
}

static avlr_node *avlr_find_predecessor(avlr_node *node, long key) {
    avlr_node *pred = NULL;
    avlr_node *curr = node;
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

static avlr_node *avlr_find_successor(avlr_node *node, long key) {
    avlr_node *succ = NULL;
    avlr_node *curr = node;
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

static long avlr_pop_min_free(avlr_alloc *a) {
    if (!a->root) {
        return -1;
    }
    avlr_node *node = a->root;
    while (node->left) {
        node = node->left;
    }
    long min_idx = node->start;
    if (node->start == node->end) {
        a->root = avlr_delete(a, a->root, node->start);
    } else {
        node->start += 1;
    }
    return min_idx;
}

static void avlr_add_free_range(avlr_alloc *a, long start, long end) {
    if (start > end) {
        return;
    }
    for (;;) {
        avlr_node *pred = avlr_find_predecessor(a->root, start);
        if (pred && pred->end + 1 >= start) {
            if (pred->start < start) {
                start = pred->start;
            }
            if (pred->end > end) {
                end = pred->end;
            }
            a->root = avlr_delete(a, a->root, pred->start);
            continue;
        }
        break;
    }
    for (;;) {
        avlr_node *succ = avlr_find_successor(a->root, end);
        if (succ && succ->start <= end + 1) {
            if (succ->end > end) {
                end = succ->end;
            }
            a->root = avlr_delete(a, a->root, succ->start);
            continue;
        }
        break;
    }
    a->root = avlr_insert(a, a->root, start, end);
}

static long avlr_allocate(allocator *self, long count, long *out) {
    avlr_alloc *a = (avlr_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    long n = 0;
    for (long i = 0; i < count; i++) {
        long idx = avlr_pop_min_free(a);
        if (idx == -1) {
            allocator_rollback(self, out, n);
            for (long j = 0; j < n; j++) {
                avlr_add_free_range(a, out[j], out[j]);
            }
            return 0;
        }
        bitmap_set(&self->bm, (size_t)idx, 1);
        out[n++] = idx;
    }
    return n;
}

static int cmp_long_asc(const void *x, const void *y) {
    long a = *(const long *)x, b = *(const long *)y;
    return (a > b) - (a < b);
}

static bool avlr_deallocate(allocator *self, const long *indices, long n) {
    avlr_alloc *a = (avlr_alloc *)self;
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
        avlr_add_free_range(a, range_start, range_end);
        range_start = sorted[i];
        range_end = sorted[i];
    }
    avlr_add_free_range(a, range_start, range_end);
    free(sorted);
    return true;
}

static long avlr_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double avlr_aux_mem(allocator *self) {
    avlr_alloc *a = (avlr_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(avlr_node));
}

static void avlr_free_nodes(avlr_node *node) {
    if (!node) {
        return;
    }
    avlr_free_nodes(node->left);
    avlr_free_nodes(node->right);
    free(node);
}

static void avlr_destroy(allocator *self) {
    avlr_alloc *a = (avlr_alloc *)self;
    avlr_free_nodes(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *avl_range_create(long size) {
    avlr_alloc *a = (avlr_alloc *)malloc(sizeof(avlr_alloc));
    allocator_base_init(&a->base, size);
    a->root = NULL;
    a->node_count = 0;
    if (size > 0) {
        a->root = avlr_new(a, 0, size - 1);
    }
    a->base.allocate = avlr_allocate;
    a->base.deallocate = avlr_deallocate;
    a->base.count_free = avlr_count_free;
    a->base.aux_mem_bytes = avlr_aux_mem;
    a->base.destroy = avlr_destroy;
    return &a->base;
}
