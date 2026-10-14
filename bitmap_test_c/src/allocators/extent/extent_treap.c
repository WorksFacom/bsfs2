/*
 * treap of free ranges with max_gap augmentation, port of treap_range_extent.py. only the structure
 * hooks live here, the policy is extent_base.c.
 */
#include <stdlib.h>

#include "allocators.h"
#include "extent.h"
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
    trnode *root;
    long node_count;
    rng_state rng;
} treap_ranges;

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

static trnode *tr_new(treap_ranges *a, long start, long end) {
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

static trnode *tr_delete(treap_ranges *a, trnode *node, long start) {
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
static int hook_find_best_fit(void *st, long count, long *s, long *e) {
    treap_ranges *a = (treap_ranges *)st;
    trnode *n = tr_best_fit(a->root, count);
    if (!n) {
        return 0;
    }
    *s = n->start;
    *e = n->end;
    return 1;
}

static int hook_pop_min_range(void *st, long *s, long *e) {
    treap_ranges *a = (treap_ranges *)st;
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

static void hook_insert_range(void *st, long start, long end) {
    treap_ranges *a = (treap_ranges *)st;
    a->root = tr_insert(a->root, tr_new(a, start, end));
}

static void hook_delete_range(void *st, long start) {
    treap_ranges *a = (treap_ranges *)st;
    a->root = tr_delete(a, a->root, start);
}

static int hook_predecessor(void *st, long key, long *s, long *e) {
    treap_ranges *a = (treap_ranges *)st;
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

static int hook_successor(void *st, long key, long *s, long *e) {
    treap_ranges *a = (treap_ranges *)st;
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

static double tr_aux_mem(void *st) {
    treap_ranges *a = (treap_ranges *)st;
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

static void tr_destroy(void *st) {
    treap_ranges *a = (treap_ranges *)st;
    tr_free(a->root);
    free(a);
}

static const extent_ops TREAP_OPS = {
    hook_find_best_fit, hook_pop_min_range, hook_insert_range, hook_delete_range,
    hook_predecessor, hook_successor, tr_aux_mem, tr_destroy,
};

allocator *treap_range_extent_create(long size) {
    treap_ranges *a = (treap_ranges *)malloc(sizeof(treap_ranges));
    a->root = NULL;
    a->node_count = 0;
    rng_seed(&a->rng, 0xE47e47);
    if (size > 0) {
        a->root = tr_new(a, 0, size - 1);
    }
    return extent_policy_create(size, &TREAP_OPS, a);
}
