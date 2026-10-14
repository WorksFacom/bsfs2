/*
 * B+tree of free ranges with max_size augmentation, port of bplustree_extent.py. only the structure
 * hooks live here, the policy is extent_base.c.
 *
 * ranges live in the leaves (parallel starts/ends), internal nodes hold separators. leaves are doubly
 * linked and every node carries max_size, the largest range below it, for best-fit search. at most
 * XB_ORDER entries per node, min fill XB_ORDER / 2.
 */
#include <stdlib.h>
#include <string.h>

#include "allocators.h"
#include "extent.h"

#define XB_ORDER 64
#define XB_MIN (XB_ORDER / 2)
#define XB_CAP (XB_ORDER + 1) /* one over max, a node splits right after it overflows */

typedef struct xbnode {
    int is_leaf;
    int nkeys;
    long max_size;
    struct xbnode *parent;
    struct xbnode *next;
    struct xbnode *prev;
    long keys[XB_CAP];
    union {
        long ends[XB_CAP];                    /* leaf */
        struct xbnode *children[XB_CAP + 1];  /* internal, nkeys + 1 of them */
    } u;
} xbnode;

typedef struct {
    xbnode *root;
    long node_count;
} bp_ranges;

static xbnode *xb_new(bp_ranges *t, int is_leaf) {
    xbnode *n = (xbnode *)calloc(1, sizeof(xbnode));
    n->is_leaf = is_leaf;
    t->node_count += 1;
    return n;
}

static int xb_bisect_left(const long *a, int n, long key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (a[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int xb_bisect_right(const long *a, int n, long key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (key < a[mid]) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

static void xb_recompute(xbnode *n) {
    long m = 0;
    if (n->is_leaf) {
        for (int j = 0; j < n->nkeys; j++) {
            long sz = n->u.ends[j] - n->keys[j] + 1;
            if (sz > m) {
                m = sz;
            }
        }
    } else {
        for (int j = 0; j <= n->nkeys; j++) {
            if (n->u.children[j]->max_size > m) {
                m = n->u.children[j]->max_size;
            }
        }
    }
    n->max_size = m;
}

/* stops at the first node whose max_size didn't change, so `n` must not be recomputed already */
static void xb_recompute_path(xbnode *n) {
    while (n) {
        long old = n->max_size;
        xb_recompute(n);
        if (n->max_size == old) {
            return;
        }
        n = n->parent;
    }
}

static void xb_recompute_path_full(xbnode *n) {
    while (n) {
        xb_recompute(n);
        n = n->parent;
    }
}

static int xb_child_index(xbnode *parent, xbnode *n) {
    for (int i = 0; i <= parent->nkeys; i++) {
        if (parent->u.children[i] == n) {
            return i;
        }
    }
    abort();
}

static xbnode *xb_find_leaf(bp_ranges *t, long start) {
    xbnode *n = t->root;
    while (!n->is_leaf) {
        n = n->u.children[xb_bisect_right(n->keys, n->nkeys, start)];
    }
    return n;
}

static xbnode *xb_leftmost_leaf(bp_ranges *t) {
    xbnode *n = t->root;
    while (!n->is_leaf) {
        n = n->u.children[0];
    }
    return n;
}

/* ----- insertion ----- */
static void xb_split_if_needed(bp_ranges *t, xbnode *n) {
    if (n->nkeys <= XB_ORDER) {
        return;
    }
    int mid = n->nkeys / 2;
    xbnode *nw = xb_new(t, n->is_leaf);
    long sep = n->keys[mid];

    if (n->is_leaf) {
        nw->nkeys = n->nkeys - mid;
        memcpy(nw->keys, &n->keys[mid], (size_t)nw->nkeys * sizeof(long));
        memcpy(nw->u.ends, &n->u.ends[mid], (size_t)nw->nkeys * sizeof(long));
        n->nkeys = mid;
        nw->next = n->next;
        nw->prev = n;
        if (n->next) {
            n->next->prev = nw;
        }
        n->next = nw;
    } else {
        nw->nkeys = n->nkeys - mid - 1;
        memcpy(nw->keys, &n->keys[mid + 1], (size_t)nw->nkeys * sizeof(long));
        memcpy(nw->u.children, &n->u.children[mid + 1], (size_t)(nw->nkeys + 1) * sizeof(xbnode *));
        n->nkeys = mid;
        for (int j = 0; j <= nw->nkeys; j++) {
            nw->u.children[j]->parent = nw;
        }
    }
    xb_recompute(n);
    xb_recompute(nw);

    xbnode *parent = n->parent;
    if (!parent) {
        parent = xb_new(t, 0);
        parent->nkeys = 1;
        parent->keys[0] = sep;
        parent->u.children[0] = n;
        parent->u.children[1] = nw;
        n->parent = parent;
        nw->parent = parent;
        t->root = parent;
        xb_recompute(parent);
        return;
    }
    int idx = xb_bisect_left(parent->keys, parent->nkeys, sep);
    memmove(&parent->keys[idx + 1], &parent->keys[idx], (size_t)(parent->nkeys - idx) * sizeof(long));
    memmove(&parent->u.children[idx + 2], &parent->u.children[idx + 1],
            (size_t)(parent->nkeys - idx) * sizeof(xbnode *));
    parent->keys[idx] = sep;
    parent->u.children[idx + 1] = nw;
    parent->nkeys += 1;
    nw->parent = parent;
    xb_recompute(parent);
    xb_split_if_needed(t, parent);
}

static void xb_insert(bp_ranges *t, long start, long end) {
    xbnode *leaf = xb_find_leaf(t, start);
    int i = xb_bisect_left(leaf->keys, leaf->nkeys, start);
    memmove(&leaf->keys[i + 1], &leaf->keys[i], (size_t)(leaf->nkeys - i) * sizeof(long));
    memmove(&leaf->u.ends[i + 1], &leaf->u.ends[i], (size_t)(leaf->nkeys - i) * sizeof(long));
    leaf->keys[i] = start;
    leaf->u.ends[i] = end;
    leaf->nkeys += 1;

    /* max_size only grows on insert, stop at the first ancestor that already covers it */
    long sz = end - start + 1;
    for (xbnode *n = leaf; n && sz > n->max_size; n = n->parent) {
        n->max_size = sz;
    }
    if (leaf->nkeys > XB_ORDER) {
        xb_split_if_needed(t, leaf);
    }
}

/* ----- deletion ----- */
static void xb_borrow_from_left(xbnode *parent, int idx) {
    xbnode *n = parent->u.children[idx];
    xbnode *left = parent->u.children[idx - 1];
    if (n->is_leaf) {
        memmove(&n->keys[1], &n->keys[0], (size_t)n->nkeys * sizeof(long));
        memmove(&n->u.ends[1], &n->u.ends[0], (size_t)n->nkeys * sizeof(long));
        left->nkeys -= 1;
        n->keys[0] = left->keys[left->nkeys];
        n->u.ends[0] = left->u.ends[left->nkeys];
        n->nkeys += 1;
        parent->keys[idx - 1] = n->keys[0];
    } else {
        memmove(&n->keys[1], &n->keys[0], (size_t)n->nkeys * sizeof(long));
        memmove(&n->u.children[1], &n->u.children[0], (size_t)(n->nkeys + 1) * sizeof(xbnode *));
        n->keys[0] = parent->keys[idx - 1];
        xbnode *moved = left->u.children[left->nkeys];
        moved->parent = n;
        n->u.children[0] = moved;
        n->nkeys += 1;
        left->nkeys -= 1;
        parent->keys[idx - 1] = left->keys[left->nkeys];
    }
    xb_recompute(left);
    xb_recompute(n);
    xb_recompute(parent);
}

static void xb_borrow_from_right(xbnode *parent, int idx) {
    xbnode *n = parent->u.children[idx];
    xbnode *right = parent->u.children[idx + 1];
    if (n->is_leaf) {
        n->keys[n->nkeys] = right->keys[0];
        n->u.ends[n->nkeys] = right->u.ends[0];
        n->nkeys += 1;
        right->nkeys -= 1;
        memmove(&right->keys[0], &right->keys[1], (size_t)right->nkeys * sizeof(long));
        memmove(&right->u.ends[0], &right->u.ends[1], (size_t)right->nkeys * sizeof(long));
        parent->keys[idx] = right->keys[0];
    } else {
        n->keys[n->nkeys] = parent->keys[idx];
        xbnode *moved = right->u.children[0];
        moved->parent = n;
        n->u.children[n->nkeys + 1] = moved;
        n->nkeys += 1;
        parent->keys[idx] = right->keys[0];
        memmove(&right->keys[0], &right->keys[1], (size_t)(right->nkeys - 1) * sizeof(long));
        memmove(&right->u.children[0], &right->u.children[1], (size_t)right->nkeys * sizeof(xbnode *));
        right->nkeys -= 1;
    }
    xb_recompute(right);
    xb_recompute(n);
    xb_recompute(parent);
}

/* merge children[j+1] into children[j] and drop it */
static void xb_merge(bp_ranges *t, xbnode *parent, int j) {
    xbnode *left = parent->u.children[j];
    xbnode *right = parent->u.children[j + 1];
    if (left->is_leaf) {
        memcpy(&left->keys[left->nkeys], right->keys, (size_t)right->nkeys * sizeof(long));
        memcpy(&left->u.ends[left->nkeys], right->u.ends, (size_t)right->nkeys * sizeof(long));
        left->nkeys += right->nkeys;
        left->next = right->next;
        if (right->next) {
            right->next->prev = left;
        }
    } else {
        left->keys[left->nkeys] = parent->keys[j];
        memcpy(&left->keys[left->nkeys + 1], right->keys, (size_t)right->nkeys * sizeof(long));
        for (int c = 0; c <= right->nkeys; c++) {
            right->u.children[c]->parent = left;
            left->u.children[left->nkeys + 1 + c] = right->u.children[c];
        }
        left->nkeys += right->nkeys + 1;
    }
    memmove(&parent->keys[j], &parent->keys[j + 1], (size_t)(parent->nkeys - j - 1) * sizeof(long));
    memmove(&parent->u.children[j + 1], &parent->u.children[j + 2],
            (size_t)(parent->nkeys - j - 1) * sizeof(xbnode *));
    parent->nkeys -= 1;
    free(right);
    t->node_count -= 1;
    xb_recompute(left);
}

static void xb_rebalance(bp_ranges *t, xbnode *n) {
    if (n == t->root) {
        if (!n->is_leaf && n->nkeys == 0) {
            t->root = n->u.children[0];
            t->root->parent = NULL;
            free(n);
            t->node_count -= 1;
        } else {
            xb_recompute(n);
        }
        return;
    }
    if (n->nkeys >= XB_MIN) {
        xb_recompute_path_full(n);
        return;
    }
    xbnode *parent = n->parent;
    int idx = xb_child_index(parent, n);
    xbnode *left = idx > 0 ? parent->u.children[idx - 1] : NULL;
    xbnode *right = idx < parent->nkeys ? parent->u.children[idx + 1] : NULL;

    if (left && left->nkeys > XB_MIN) {
        xb_borrow_from_left(parent, idx);
        xb_recompute_path_full(parent);
        return;
    }
    if (right && right->nkeys > XB_MIN) {
        xb_borrow_from_right(parent, idx);
        xb_recompute_path_full(parent);
        return;
    }
    xb_merge(t, parent, left ? idx - 1 : idx);
    xb_recompute(parent);
    xb_rebalance(t, parent);
}

static void xb_delete(bp_ranges *t, long start) {
    xbnode *leaf = xb_find_leaf(t, start);
    int i = xb_bisect_left(leaf->keys, leaf->nkeys, start);
    if (i >= leaf->nkeys || leaf->keys[i] != start) {
        return;
    }
    long removed = leaf->u.ends[i] - leaf->keys[i] + 1;
    memmove(&leaf->keys[i], &leaf->keys[i + 1], (size_t)(leaf->nkeys - i - 1) * sizeof(long));
    memmove(&leaf->u.ends[i], &leaf->u.ends[i + 1], (size_t)(leaf->nkeys - i - 1) * sizeof(long));
    leaf->nkeys -= 1;

    if (leaf->nkeys >= XB_MIN || leaf == t->root) {
        /* no structural change, max_size only drops if a maximal entry went */
        if (removed >= leaf->max_size) {
            xb_recompute(leaf);
            xb_recompute_path(leaf->parent);
        }
        return;
    }
    xb_recompute(leaf);
    xb_rebalance(t, leaf);
}

static int xb_best_fit(xbnode *n, long count, long *bs, long *bstart, long *bend) {
    if (n->max_size < count) {
        return 0;
    }
    int found = 0;
    if (n->is_leaf) {
        for (int j = 0; j < n->nkeys; j++) {
            long sz = n->u.ends[j] - n->keys[j] + 1;
            if (sz >= count && (*bs < 0 || sz < *bs)) {
                *bs = sz;
                *bstart = n->keys[j];
                *bend = n->u.ends[j];
                found = 1;
            }
        }
        return found;
    }
    for (int c = 0; c <= n->nkeys; c++) {
        if (n->u.children[c]->max_size >= count) {
            found |= xb_best_fit(n->u.children[c], count, bs, bstart, bend);
        }
    }
    return found;
}

/* ----- structure hooks ----- */
static int hook_find_best_fit(void *st, long count, long *s, long *e) {
    long bs = -1;
    return xb_best_fit(((bp_ranges *)st)->root, count, &bs, s, e);
}

static int hook_pop_min_range(void *st, long *s, long *e) {
    bp_ranges *t = (bp_ranges *)st;
    xbnode *leaf = xb_leftmost_leaf(t);
    if (leaf->nkeys == 0) {
        return 0;
    }
    *s = leaf->keys[0];
    *e = leaf->u.ends[0];
    xb_delete(t, *s);
    return 1;
}

static void hook_insert_range(void *st, long start, long end) {
    xb_insert((bp_ranges *)st, start, end);
}

static void hook_delete_range(void *st, long start) {
    xb_delete((bp_ranges *)st, start);
}

static int hook_predecessor(void *st, long key, long *s, long *e) {
    xbnode *leaf = xb_find_leaf((bp_ranges *)st, key);
    int i = xb_bisect_left(leaf->keys, leaf->nkeys, key) - 1;
    if (i >= 0) {
        *s = leaf->keys[i];
        *e = leaf->u.ends[i];
        return 1;
    }
    xbnode *n = leaf->prev;
    while (n && n->nkeys == 0) {
        n = n->prev;
    }
    if (!n) {
        return 0;
    }
    *s = n->keys[n->nkeys - 1];
    *e = n->u.ends[n->nkeys - 1];
    return 1;
}

static int hook_successor(void *st, long key, long *s, long *e) {
    xbnode *leaf = xb_find_leaf((bp_ranges *)st, key);
    int i = xb_bisect_right(leaf->keys, leaf->nkeys, key);
    if (i < leaf->nkeys) {
        *s = leaf->keys[i];
        *e = leaf->u.ends[i];
        return 1;
    }
    xbnode *n = leaf->next;
    while (n && n->nkeys == 0) {
        n = n->next;
    }
    if (!n) {
        return 0;
    }
    *s = n->keys[0];
    *e = n->u.ends[0];
    return 1;
}

static double xb_aux_mem(void *st) {
    return (double)((size_t)((bp_ranges *)st)->node_count * sizeof(xbnode));
}

static void xb_free(xbnode *n) {
    if (!n->is_leaf) {
        for (int c = 0; c <= n->nkeys; c++) {
            xb_free(n->u.children[c]);
        }
    }
    free(n);
}

static void xb_destroy(void *st) {
    xb_free(((bp_ranges *)st)->root);
    free(st);
}

static const extent_ops BPLUS_OPS = {
    hook_find_best_fit, hook_pop_min_range, hook_insert_range, hook_delete_range,
    hook_predecessor, hook_successor, xb_aux_mem, xb_destroy,
};

allocator *bplustree_extent_create(long size) {
    bp_ranges *t = (bp_ranges *)malloc(sizeof(bp_ranges));
    t->node_count = 0;
    t->root = xb_new(t, 1);
    if (size > 0) {
        t->root->keys[0] = 0;
        t->root->u.ends[0] = size - 1;
        t->root->nkeys = 1;
        t->root->max_size = size;
    }
    return extent_policy_create(size, &BPLUS_OPS, t);
}
