/*
 * lazy allocator, B+tree (order 256) of recycled blocks + next_fresh. port of
 * allocators/scatter/bplustree.py. leaves are doubly linked, first_leaf caches the smallest recycled index.
 */
#include <stdlib.h>
#include <string.h>

#include "allocators.h"

#define BP_ORDER 256
#define BP_MAXK (2 * BP_ORDER)
#define BP_MAXC (2 * BP_ORDER + 1)

typedef struct bpnode {
    int is_leaf;
    int nkeys;
    int nchild;
    long keys[BP_MAXK];
    struct bpnode *children[BP_MAXC];
    struct bpnode *next;
    struct bpnode *prev;
} bpnode;

typedef struct {
    allocator base;
    bpnode *root;
    bpnode *first_leaf;
    long next_fresh;
    long node_count;
} bp_alloc;

static bpnode *bp_new(bp_alloc *a, int is_leaf) {
    bpnode *n = (bpnode *)malloc(sizeof(bpnode));
    n->is_leaf = is_leaf;
    n->nkeys = 0;
    n->nchild = 0;
    n->next = NULL;
    n->prev = NULL;
    a->node_count += 1;
    return n;
}

static int bp_is_full(bpnode *n) {
    return n->nkeys >= 2 * BP_ORDER - 1;
}

static int bp_bisect_left(const long *keys, int n, long key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void bp_keys_insert(bpnode *n, int i, long key) {
    memmove(&n->keys[i + 1], &n->keys[i], (size_t)(n->nkeys - i) * sizeof(long));
    n->keys[i] = key;
    n->nkeys += 1;
}

static long bp_keys_pop(bpnode *n, int i) {
    long v = n->keys[i];
    memmove(&n->keys[i], &n->keys[i + 1], (size_t)(n->nkeys - i - 1) * sizeof(long));
    n->nkeys -= 1;
    return v;
}

static void bp_child_insert(bpnode *n, int i, bpnode *c) {
    memmove(&n->children[i + 1], &n->children[i], (size_t)(n->nchild - i) * sizeof(bpnode *));
    n->children[i] = c;
    n->nchild += 1;
}

static bpnode *bp_child_pop(bpnode *n, int i) {
    bpnode *c = n->children[i];
    memmove(&n->children[i], &n->children[i + 1], (size_t)(n->nchild - i - 1) * sizeof(bpnode *));
    n->nchild -= 1;
    return c;
}

static void bp_split_child(bp_alloc *a, bpnode *parent, int index) {
    bpnode *full_child = parent->children[index];
    bpnode *new_child = bp_new(a, full_child->is_leaf);
    int mid_index = BP_ORDER - 1;

    if (full_child->is_leaf) {
        int tail = full_child->nkeys - mid_index;
        memcpy(new_child->keys, &full_child->keys[mid_index], (size_t)tail * sizeof(long));
        new_child->nkeys = tail;
        full_child->nkeys = mid_index;

        new_child->next = full_child->next;
        new_child->prev = full_child;
        if (full_child->next) {
            full_child->next->prev = new_child;
        }
        full_child->next = new_child;

        bp_keys_insert(parent, index, new_child->keys[0]);
    } else {
        long promoted_key = full_child->keys[mid_index];
        int ktail = full_child->nkeys - (mid_index + 1);
        memcpy(new_child->keys, &full_child->keys[mid_index + 1], (size_t)ktail * sizeof(long));
        new_child->nkeys = ktail;
        int ctail = full_child->nchild - (mid_index + 1);
        memcpy(new_child->children, &full_child->children[mid_index + 1], (size_t)ctail * sizeof(bpnode *));
        new_child->nchild = ctail;

        full_child->nkeys = mid_index;
        full_child->nchild = mid_index + 1;

        bp_keys_insert(parent, index, promoted_key);
    }
    bp_child_insert(parent, index + 1, new_child);
}

static void bp_insert_non_full(bp_alloc *a, bpnode *node, long key) {
    if (node->is_leaf) {
        int i = bp_bisect_left(node->keys, node->nkeys, key);
        bp_keys_insert(node, i, key);
    } else {
        int i = bp_bisect_left(node->keys, node->nkeys, key);
        bpnode *child = node->children[i];
        if (bp_is_full(child)) {
            bp_split_child(a, node, i);
            if (key > node->keys[i]) {
                i += 1;
            }
            child = node->children[i];
        }
        bp_insert_non_full(a, child, key);
    }
}

static void bp_insert_key(bp_alloc *a, long key) {
    if (bp_is_full(a->root)) {
        bpnode *new_root = bp_new(a, 0);
        new_root->children[0] = a->root;
        new_root->nchild = 1;
        bp_split_child(a, new_root, 0);
        a->root = new_root;
    }
    bp_insert_non_full(a, a->root, key);
}

static void bp_fill(bp_alloc *a, bpnode *node, int index);

static void bp_delete_from_node(bp_alloc *a, bpnode *node, long key) {
    int i = bp_bisect_left(node->keys, node->nkeys, key);

    if (node->is_leaf) {
        if (i < node->nkeys && node->keys[i] == key) {
            bp_keys_pop(node, i);
        }
        return;
    }

    if (i < node->nkeys && node->keys[i] == key) {
        bpnode *child = node->children[i];
        if (child->nkeys >= BP_ORDER) {
            long predecessor = child->keys[child->nkeys - 1];
            node->keys[i] = predecessor;
            bp_delete_from_node(a, child, predecessor);
        } else {
            bpnode *sibling = node->children[i + 1];
            child->keys[child->nkeys++] = node->keys[i];
            memcpy(&child->keys[child->nkeys], sibling->keys, (size_t)sibling->nkeys * sizeof(long));
            child->nkeys += sibling->nkeys;

            if (!child->is_leaf) {
                memcpy(&child->children[child->nchild], sibling->children, (size_t)sibling->nchild * sizeof(bpnode *));
                child->nchild += sibling->nchild;
            } else {
                child->next = sibling->next;
                if (sibling->next) {
                    sibling->next->prev = child;
                }
            }
            bp_keys_pop(node, i);
            bp_child_pop(node, i + 1);
            free(sibling);
            a->node_count -= 1;
            bp_delete_from_node(a, child, key);
        }
    } else {
        bpnode *child = node->children[i];
        if (child->nkeys < BP_ORDER) {
            bp_fill(a, node, i);
        }
        if (i > node->nkeys) {
            bp_delete_from_node(a, node->children[i - 1], key);
        } else {
            bp_delete_from_node(a, node->children[i], key);
        }
    }
}

static void bp_borrow_from_prev(bpnode *node, int child_index) {
    bpnode *child = node->children[child_index];
    bpnode *sibling = node->children[child_index - 1];

    bp_keys_insert(child, 0, bp_keys_pop(sibling, sibling->nkeys - 1));
    if (!child->is_leaf) {
        bpnode *moved = bp_child_pop(sibling, sibling->nchild - 1);
        bp_child_insert(child, 0, moved);
    }
    node->keys[child_index - 1] = child->keys[0];
}

static void bp_borrow_from_next(bpnode *node, int child_index) {
    bpnode *child = node->children[child_index];
    bpnode *sibling = node->children[child_index + 1];

    child->keys[child->nkeys++] = bp_keys_pop(sibling, 0);
    if (!child->is_leaf) {
        bpnode *moved = bp_child_pop(sibling, 0);
        child->children[child->nchild++] = moved;
    }
    node->keys[child_index] = sibling->keys[0];
}

static void bp_merge(bp_alloc *a, bpnode *node, int index) {
    bpnode *child = node->children[index];
    bpnode *sibling = node->children[index + 1];

    if (child->is_leaf) {
        memcpy(&child->keys[child->nkeys], sibling->keys, (size_t)sibling->nkeys * sizeof(long));
        child->nkeys += sibling->nkeys;
        child->next = sibling->next;
        if (sibling->next) {
            sibling->next->prev = child;
        }
    } else {
        child->keys[child->nkeys++] = node->keys[index];
        memcpy(&child->keys[child->nkeys], sibling->keys, (size_t)sibling->nkeys * sizeof(long));
        child->nkeys += sibling->nkeys;
        memcpy(&child->children[child->nchild], sibling->children, (size_t)sibling->nchild * sizeof(bpnode *));
        child->nchild += sibling->nchild;
    }
    bp_keys_pop(node, index);
    bp_child_pop(node, index + 1);
    free(sibling);
    a->node_count -= 1;
}

static void bp_fill(bp_alloc *a, bpnode *node, int index) {
    if (index > 0 && node->children[index - 1]->nkeys >= BP_ORDER) {
        bp_borrow_from_prev(node, index);
    } else if (index < node->nchild - 1 && node->children[index + 1]->nkeys >= BP_ORDER) {
        bp_borrow_from_next(node, index);
    } else {
        if (index < node->nchild - 1) {
            bp_merge(a, node, index);
        } else {
            bp_merge(a, node, index - 1);
        }
    }
}

static void bp_delete_key(bp_alloc *a, long key) {
    bp_delete_from_node(a, a->root, key);

    if (a->root->nkeys == 0 && a->root->nchild > 0) {
        bpnode *old = a->root;
        a->root = a->root->children[0];
        free(old);
        a->node_count -= 1;
    }

    /* only recompute first_leaf if the minimum went away */
    if (a->first_leaf && a->first_leaf->nkeys > 0 && key == a->first_leaf->keys[0]) {
        a->first_leaf = a->first_leaf->next ? a->first_leaf->next : a->first_leaf;
    } else if (a->first_leaf && a->first_leaf->nkeys == 0 && a->first_leaf->next) {
        a->first_leaf = a->first_leaf->next;
    } else if (!a->first_leaf || a->first_leaf->nkeys == 0) {
        bpnode *curr = a->root;
        while (!curr->is_leaf) {
            curr = curr->children[0];
        }
        a->first_leaf = curr;
    }
}

static long bp_find_free_block(bp_alloc *a) {
    long tree_min = -1;
    bpnode *leaf = a->first_leaf;
    if (leaf && leaf->nkeys > 0) {
        tree_min = leaf->keys[0];
    }
    long fresh_min = (a->next_fresh < a->base.size) ? a->next_fresh : -1;
    if (tree_min == -1) {
        return fresh_min;
    }
    if (fresh_min == -1) {
        return tree_min;
    }
    return tree_min < fresh_min ? tree_min : fresh_min;
}

static long bplustree_allocate(allocator *self, long count, long *out) {
    bp_alloc *a = (bp_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long idx = bp_find_free_block(a);
        if (idx == -1) {
            return 0;
        }
        bitmap_set(&self->bm, (size_t)idx, 1);
        if (idx == a->next_fresh) {
            a->next_fresh += 1;
        } else {
            bp_delete_key(a, idx);
        }
        out[i] = idx;
    }
    return count;
}

static bool bplustree_deallocate(allocator *self, const long *indices, long n) {
    bp_alloc *a = (bp_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        bp_insert_key(a, indices[i]);
    }
    return true;
}

static long bplustree_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double bplustree_aux_mem(allocator *self) {
    bp_alloc *a = (bp_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(bpnode));
}

static void bp_free_nodes(bpnode *node) {
    if (!node) {
        return;
    }
    if (!node->is_leaf) {
        for (int i = 0; i < node->nchild; i++) {
            bp_free_nodes(node->children[i]);
        }
    }
    free(node);
}

static void bplustree_destroy(allocator *self) {
    bp_alloc *a = (bp_alloc *)self;
    bp_free_nodes(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *bplustree_create(long size) {
    bp_alloc *a = (bp_alloc *)malloc(sizeof(bp_alloc));
    allocator_base_init(&a->base, size);
    a->root = NULL;
    a->next_fresh = 0;
    a->node_count = 0;
    a->root = bp_new(a, 1);
    a->first_leaf = a->root;
    a->base.allocate = bplustree_allocate;
    a->base.deallocate = bplustree_deallocate;
    a->base.count_free = bplustree_count_free;
    a->base.aux_mem_bytes = bplustree_aux_mem;
    a->base.destroy = bplustree_destroy;
    return &a->base;
}
