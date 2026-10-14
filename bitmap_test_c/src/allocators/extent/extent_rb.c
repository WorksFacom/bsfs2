/*
 * red-black tree of free ranges with max_gap augmentation, port of rb_range_extent.py. only the
 * structure hooks live here, the policy is extent_base.c.
 *
 * rotations recompute their two pivots, every raw link change propagates max_gap to the root before
 * the fixup runs, and recoloring never touches max_gap.
 */
#include <stdlib.h>

#include "allocators.h"
#include "extent.h"

#define XR_RED 0
#define XR_BLACK 1

typedef struct xrnode {
    long start;
    long end;
    long max_gap;
    struct xrnode *left;
    struct xrnode *right;
    struct xrnode *parent;
    int color;
} xrnode;

typedef struct {
    xrnode *root;
    long node_count;
} rb_ranges;

static int xr_color(xrnode *n) {
    return n ? n->color : XR_BLACK;
}

static void xr_update(xrnode *node) {
    long m = node->end - node->start + 1;
    if (node->left && node->left->max_gap > m) {
        m = node->left->max_gap;
    }
    if (node->right && node->right->max_gap > m) {
        m = node->right->max_gap;
    }
    node->max_gap = m;
}

/* stops at the first unchanged ancestor, except it always walks through `force` (the node that took
 * its successor's range in a two-child delete) */
static void xr_update_to_root(xrnode *node, xrnode *force) {
    while (node) {
        long old = node->max_gap;
        xr_update(node);
        if (node == force) {
            force = NULL;
        }
        if (!force && node->max_gap == old) {
            return;
        }
        node = node->parent;
    }
}

static void xr_rotate_left(rb_ranges *t, xrnode *node) {
    xrnode *rc = node->right;
    node->right = rc->left;
    if (rc->left) {
        rc->left->parent = node;
    }
    rc->parent = node->parent;
    if (!node->parent) {
        t->root = rc;
    } else if (node->parent->left == node) {
        node->parent->left = rc;
    } else {
        node->parent->right = rc;
    }
    rc->left = node;
    node->parent = rc;
    xr_update(node);
    xr_update(rc);
}

static void xr_rotate_right(rb_ranges *t, xrnode *node) {
    xrnode *lc = node->left;
    node->left = lc->right;
    if (lc->right) {
        lc->right->parent = node;
    }
    lc->parent = node->parent;
    if (!node->parent) {
        t->root = lc;
    } else if (node->parent->left == node) {
        node->parent->left = lc;
    } else {
        node->parent->right = lc;
    }
    lc->right = node;
    node->parent = lc;
    xr_update(node);
    xr_update(lc);
}

static void xr_fix_insert(rb_ranges *t, xrnode *node) {
    while (node->parent && node->parent->color == XR_RED) {
        xrnode *gp = node->parent->parent;
        if (!gp) {
            break;
        }
        if (node->parent == gp->left) {
            xrnode *uncle = gp->right;
            if (xr_color(uncle) == XR_RED) {
                node->parent->color = XR_BLACK;
                uncle->color = XR_BLACK;
                gp->color = XR_RED;
                node = gp;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    xr_rotate_left(t, node);
                }
                node->parent->color = XR_BLACK;
                node->parent->parent->color = XR_RED;
                xr_rotate_right(t, node->parent->parent);
            }
        } else {
            xrnode *uncle = gp->left;
            if (xr_color(uncle) == XR_RED) {
                node->parent->color = XR_BLACK;
                uncle->color = XR_BLACK;
                gp->color = XR_RED;
                node = gp;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    xr_rotate_right(t, node);
                }
                node->parent->color = XR_BLACK;
                node->parent->parent->color = XR_RED;
                xr_rotate_left(t, node->parent->parent);
            }
        }
    }
    t->root->color = XR_BLACK;
}

/* CLRS delete fixup. `node` (maybe NULL) carries the extra black, `is_left` is its side under `parent` */
static void xr_fix_delete(rb_ranges *t, xrnode *node, xrnode *parent, int is_left) {
    while (node != t->root && xr_color(node) == XR_BLACK) {
        if (!parent) {
            break;
        }
        if (is_left) {
            xrnode *sibling = parent->right;
            if (xr_color(sibling) == XR_RED) {
                sibling->color = XR_BLACK;
                parent->color = XR_RED;
                xr_rotate_left(t, parent);
                sibling = parent->right;
            }
            if (!sibling) {
                node = parent;
                parent = node->parent;
                is_left = parent && parent->left == node;
                continue;
            }
            if (xr_color(sibling->left) == XR_BLACK && xr_color(sibling->right) == XR_BLACK) {
                sibling->color = XR_RED;
                node = parent;
                parent = node->parent;
                is_left = parent && parent->left == node;
            } else {
                if (xr_color(sibling->right) == XR_BLACK) {
                    sibling->left->color = XR_BLACK;
                    sibling->color = XR_RED;
                    xr_rotate_right(t, sibling);
                    sibling = parent->right;
                }
                sibling->color = parent->color;
                parent->color = XR_BLACK;
                if (sibling->right) {
                    sibling->right->color = XR_BLACK;
                }
                xr_rotate_left(t, parent);
                node = t->root;
                parent = NULL;
            }
        } else {
            xrnode *sibling = parent->left;
            if (xr_color(sibling) == XR_RED) {
                sibling->color = XR_BLACK;
                parent->color = XR_RED;
                xr_rotate_right(t, parent);
                sibling = parent->left;
            }
            if (!sibling) {
                node = parent;
                parent = node->parent;
                is_left = parent && parent->left == node;
                continue;
            }
            if (xr_color(sibling->left) == XR_BLACK && xr_color(sibling->right) == XR_BLACK) {
                sibling->color = XR_RED;
                node = parent;
                parent = node->parent;
                is_left = parent && parent->left == node;
            } else {
                if (xr_color(sibling->left) == XR_BLACK) {
                    sibling->right->color = XR_BLACK;
                    sibling->color = XR_RED;
                    xr_rotate_left(t, sibling);
                    sibling = parent->left;
                }
                sibling->color = parent->color;
                parent->color = XR_BLACK;
                if (sibling->left) {
                    sibling->left->color = XR_BLACK;
                }
                xr_rotate_right(t, parent);
                node = t->root;
                parent = NULL;
            }
        }
    }
    if (node) {
        node->color = XR_BLACK;
    }
}

static xrnode *xr_find_min(xrnode *n) {
    if (!n) {
        return NULL;
    }
    while (n->left) {
        n = n->left;
    }
    return n;
}

static xrnode *xr_find(xrnode *n, long start) {
    while (n) {
        if (start == n->start) {
            return n;
        }
        n = start < n->start ? n->left : n->right;
    }
    return NULL;
}

static void xr_insert(rb_ranges *t, long start, long end) {
    xrnode *nn = (xrnode *)malloc(sizeof(xrnode));
    nn->start = start;
    nn->end = end;
    nn->max_gap = end - start + 1;
    nn->left = nn->right = nn->parent = NULL;
    nn->color = XR_RED;

    if (!t->root) {
        nn->color = XR_BLACK;
        t->root = nn;
        t->node_count += 1;
        return;
    }
    xrnode *parent = NULL;
    xrnode *cur = t->root;
    while (cur) {
        parent = cur;
        if (start < cur->start) {
            cur = cur->left;
        } else if (start > cur->start) {
            cur = cur->right;
        } else {
            free(nn);
            return;
        }
    }
    nn->parent = parent;
    if (start < parent->start) {
        parent->left = nn;
    } else {
        parent->right = nn;
    }
    t->node_count += 1;
    /* the new leaf's max_gap is already right, only ancestors can change */
    xr_update_to_root(parent, NULL);
    xr_fix_insert(t, nn);
}

static void xr_delete(rb_ranges *t, long start) {
    xrnode *node = xr_find(t->root, start);
    if (!node) {
        return;
    }
    xrnode *value_changed = NULL;
    if (node->left && node->right) {
        xrnode *succ = xr_find_min(node->right);
        node->start = succ->start;
        node->end = succ->end;
        value_changed = node;
        node = succ;
    }
    xrnode *child = node->left ? node->left : node->right;
    if (child) {
        child->parent = node->parent;
    }
    int is_left = node->parent && node->parent->left == node;
    if (!node->parent) {
        t->root = child;
    } else if (is_left) {
        node->parent->left = child;
    } else {
        node->parent->right = child;
    }
    xr_update_to_root(node->parent, value_changed);
    if (node->color == XR_BLACK) {
        xr_fix_delete(t, child, node->parent, is_left);
    }
    free(node);
    t->node_count -= 1;
}

static xrnode *xr_best_fit(xrnode *node, long count) {
    if (!node || node->max_gap < count) {
        return NULL;
    }
    xrnode *best = NULL;
    if (node->left && node->left->max_gap >= count) {
        best = xr_best_fit(node->left, count);
    }
    long sz = node->end - node->start + 1;
    if (sz >= count && (!best || sz < best->end - best->start + 1)) {
        best = node;
    }
    if (node->right && node->right->max_gap >= count) {
        xrnode *rb = xr_best_fit(node->right, count);
        if (rb && (!best || rb->end - rb->start + 1 < best->end - best->start + 1)) {
            best = rb;
        }
    }
    return best;
}

/* ----- structure hooks ----- */
static int hook_find_best_fit(void *st, long count, long *s, long *e) {
    xrnode *n = xr_best_fit(((rb_ranges *)st)->root, count);
    if (!n) {
        return 0;
    }
    *s = n->start;
    *e = n->end;
    return 1;
}

static int hook_pop_min_range(void *st, long *s, long *e) {
    rb_ranges *t = (rb_ranges *)st;
    xrnode *n = xr_find_min(t->root);
    if (!n) {
        return 0;
    }
    *s = n->start;
    *e = n->end;
    xr_delete(t, n->start);
    return 1;
}

static void hook_insert_range(void *st, long start, long end) {
    xr_insert((rb_ranges *)st, start, end);
}

static void hook_delete_range(void *st, long start) {
    xr_delete((rb_ranges *)st, start);
}

static int hook_predecessor(void *st, long key, long *s, long *e) {
    xrnode *pred = NULL;
    xrnode *cur = ((rb_ranges *)st)->root;
    while (cur) {
        if (cur->start < key) {
            pred = cur;
            cur = cur->right;
        } else {
            cur = cur->left;
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
    xrnode *succ = NULL;
    xrnode *cur = ((rb_ranges *)st)->root;
    while (cur) {
        if (cur->start > key) {
            succ = cur;
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }
    if (!succ) {
        return 0;
    }
    *s = succ->start;
    *e = succ->end;
    return 1;
}

static double xr_aux_mem(void *st) {
    return (double)((size_t)((rb_ranges *)st)->node_count * sizeof(xrnode));
}

static void xr_free(xrnode *n) {
    if (!n) {
        return;
    }
    xr_free(n->left);
    xr_free(n->right);
    free(n);
}

static void xr_destroy(void *st) {
    xr_free(((rb_ranges *)st)->root);
    free(st);
}

static const extent_ops RB_OPS = {
    hook_find_best_fit, hook_pop_min_range, hook_insert_range, hook_delete_range,
    hook_predecessor, hook_successor, xr_aux_mem, xr_destroy,
};

allocator *rb_range_extent_create(long size) {
    rb_ranges *t = (rb_ranges *)malloc(sizeof(rb_ranges));
    t->root = NULL;
    t->node_count = 0;
    if (size > 0) {
        xr_insert(t, 0, size - 1);
    }
    return extent_policy_create(size, &RB_OPS, t);
}
