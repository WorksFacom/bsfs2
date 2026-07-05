/*
 * Lazy allocator using a B-Tree (order 256) of recycled blocks + next_fresh.
 * Port of allocators/scatter/btree.py.
 */
#include <stdlib.h>
#include <string.h>

#include "allocators.h"

#define BT_ORDER 256
#define BT_MAXK (2 * BT_ORDER)     /* headroom over 2*order-1 */
#define BT_MAXC (2 * BT_ORDER + 1)

typedef struct bnode {
    int is_leaf;
    int nkeys;
    int nchild;
    long keys[BT_MAXK];
    struct bnode *children[BT_MAXC];
} bnode;

typedef struct {
    allocator base;
    bnode *root;
    long next_fresh;
    long node_count;
} btree_alloc;

static bnode *bt_new(btree_alloc *a, int is_leaf) {
    bnode *n = (bnode *)malloc(sizeof(bnode));
    n->is_leaf = is_leaf;
    n->nkeys = 0;
    n->nchild = 0;
    a->node_count += 1;
    return n;
}

static int bt_is_full(bnode *n) {
    return n->nkeys >= 2 * BT_ORDER - 1;
}

/* bisect_left over keys */
static int bt_bisect_left(const long *keys, int n, long key) {
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

static void keys_insert(bnode *n, int i, long key) {
    memmove(&n->keys[i + 1], &n->keys[i], (size_t)(n->nkeys - i) * sizeof(long));
    n->keys[i] = key;
    n->nkeys += 1;
}

static long keys_pop(bnode *n, int i) {
    long v = n->keys[i];
    memmove(&n->keys[i], &n->keys[i + 1], (size_t)(n->nkeys - i - 1) * sizeof(long));
    n->nkeys -= 1;
    return v;
}

static void child_insert(bnode *n, int i, bnode *c) {
    memmove(&n->children[i + 1], &n->children[i], (size_t)(n->nchild - i) * sizeof(bnode *));
    n->children[i] = c;
    n->nchild += 1;
}

static bnode *child_pop(bnode *n, int i) {
    bnode *c = n->children[i];
    memmove(&n->children[i], &n->children[i + 1], (size_t)(n->nchild - i - 1) * sizeof(bnode *));
    n->nchild -= 1;
    return c;
}

static void bt_split_child(btree_alloc *a, bnode *parent, int index) {
    bnode *full_child = parent->children[index];
    bnode *new_child = bt_new(a, full_child->is_leaf);

    int mid_index = BT_ORDER - 1;
    long mid_key = full_child->keys[mid_index];

    /* new_child.keys = full_child.keys[mid+1:] */
    int tail = full_child->nkeys - (mid_index + 1);
    memcpy(new_child->keys, &full_child->keys[mid_index + 1], (size_t)tail * sizeof(long));
    new_child->nkeys = tail;
    /* full_child.keys = full_child.keys[:mid] */
    full_child->nkeys = mid_index;

    if (!full_child->is_leaf) {
        int ctail = full_child->nchild - (mid_index + 1);
        memcpy(new_child->children, &full_child->children[mid_index + 1], (size_t)ctail * sizeof(bnode *));
        new_child->nchild = ctail;
        full_child->nchild = mid_index + 1;
    }

    keys_insert(parent, index, mid_key);
    child_insert(parent, index + 1, new_child);
}

static void bt_insert_non_full(btree_alloc *a, bnode *node, long key) {
    if (node->is_leaf) {
        int i = bt_bisect_left(node->keys, node->nkeys, key);
        keys_insert(node, i, key);
    } else {
        int i = bt_bisect_left(node->keys, node->nkeys, key);
        bnode *child = node->children[i];
        if (bt_is_full(child)) {
            bt_split_child(a, node, i);
            if (key > node->keys[i]) {
                i += 1;
            }
        }
        bt_insert_non_full(a, node->children[i], key);
    }
}

static void bt_insert_key(btree_alloc *a, long key) {
    if (bt_is_full(a->root)) {
        bnode *new_root = bt_new(a, 0);
        new_root->children[0] = a->root;
        new_root->nchild = 1;
        bt_split_child(a, new_root, 0);
        a->root = new_root;
    }
    bt_insert_non_full(a, a->root, key);
}

static long bt_get_predecessor(bnode *node, int index) {
    bnode *curr = node->children[index];
    while (!curr->is_leaf) {
        curr = curr->children[curr->nchild - 1];
    }
    return curr->keys[curr->nkeys - 1];
}

static long bt_get_successor(bnode *node, int index) {
    bnode *curr = node->children[index + 1];
    while (!curr->is_leaf) {
        curr = curr->children[0];
    }
    return curr->keys[0];
}

static void bt_merge(btree_alloc *a, bnode *node, int index) {
    bnode *child = node->children[index];
    bnode *sibling = node->children[index + 1];

    child->keys[child->nkeys++] = keys_pop(node, index);
    memcpy(&child->keys[child->nkeys], sibling->keys, (size_t)sibling->nkeys * sizeof(long));
    child->nkeys += sibling->nkeys;

    if (!child->is_leaf) {
        memcpy(&child->children[child->nchild], sibling->children, (size_t)sibling->nchild * sizeof(bnode *));
        child->nchild += sibling->nchild;
    }

    child_pop(node, index + 1);
    free(sibling);
    a->node_count -= 1;
}

static void bt_borrow_from_prev(bnode *node, int child_index) {
    bnode *child = node->children[child_index];
    bnode *sibling = node->children[child_index - 1];

    keys_insert(child, 0, node->keys[child_index - 1]);
    node->keys[child_index - 1] = keys_pop(sibling, sibling->nkeys - 1);

    if (!child->is_leaf) {
        bnode *moved = child_pop(sibling, sibling->nchild - 1);
        child_insert(child, 0, moved);
    }
}

static void bt_borrow_from_next(bnode *node, int child_index) {
    bnode *child = node->children[child_index];
    bnode *sibling = node->children[child_index + 1];

    child->keys[child->nkeys++] = node->keys[child_index];
    node->keys[child_index] = keys_pop(sibling, 0);

    if (!child->is_leaf) {
        bnode *moved = child_pop(sibling, 0);
        child->children[child->nchild++] = moved;
    }
}

static void bt_fill(btree_alloc *a, bnode *node, int index) {
    if (index != 0 && node->children[index - 1]->nkeys >= BT_ORDER) {
        bt_borrow_from_prev(node, index);
    } else if (index != node->nchild - 1 && node->children[index + 1]->nkeys >= BT_ORDER) {
        bt_borrow_from_next(node, index);
    } else {
        if (index != node->nchild - 1) {
            bt_merge(a, node, index);
        } else {
            bt_merge(a, node, index - 1);
        }
    }
}

static void bt_delete_from_node(btree_alloc *a, bnode *node, long key);

static void bt_delete_internal_node(btree_alloc *a, bnode *node, long key, int index) {
    long key_to_delete = node->keys[index];
    (void)key;

    if (node->children[index]->nkeys >= BT_ORDER) {
        long predecessor = bt_get_predecessor(node, index);
        node->keys[index] = predecessor;
        bt_delete_from_node(a, node->children[index], predecessor);
    } else if (node->children[index + 1]->nkeys >= BT_ORDER) {
        long successor = bt_get_successor(node, index);
        node->keys[index] = successor;
        bt_delete_from_node(a, node->children[index + 1], successor);
    } else {
        bt_merge(a, node, index);
        bt_delete_from_node(a, node->children[index], key_to_delete);
    }
}

static void bt_delete_from_node(btree_alloc *a, bnode *node, long key) {
    int i = bt_bisect_left(node->keys, node->nkeys, key);

    if (i < node->nkeys && key == node->keys[i]) {
        if (node->is_leaf) {
            keys_pop(node, i);
        } else {
            bt_delete_internal_node(a, node, key, i);
        }
    } else if (!node->is_leaf) {
        int is_in_subtree = (i == node->nkeys);

        if (node->children[i]->nkeys < BT_ORDER) {
            bt_fill(a, node, i);
        }

        if (is_in_subtree && i > node->nkeys) {
            bt_delete_from_node(a, node->children[i - 1], key);
        } else {
            bt_delete_from_node(a, node->children[i], key);
        }
    }
}

static void bt_delete_key(btree_alloc *a, long key) {
    bt_delete_from_node(a, a->root, key);
    if (a->root->nkeys == 0 && a->root->nchild > 0) {
        bnode *old = a->root;
        a->root = a->root->children[0];
        free(old);
        a->node_count -= 1;
    }
}

static long bt_find_free_block(btree_alloc *a) {
    long tree_min = -1;
    if (a->root && a->root->nkeys > 0) {
        bnode *node = a->root;
        while (!node->is_leaf) {
            node = node->children[0];
        }
        tree_min = node->nkeys > 0 ? node->keys[0] : -1;
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

static long btree_allocate(allocator *self, long count, long *out) {
    btree_alloc *a = (btree_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long idx = bt_find_free_block(a);
        if (idx == -1) {
            return 0;
        }
        bitmap_set(&self->bm, (size_t)idx, 1);
        if (idx == a->next_fresh) {
            a->next_fresh += 1;
        } else {
            bt_delete_key(a, idx);
        }
        out[i] = idx;
    }
    return count;
}

static bool btree_deallocate(allocator *self, const long *indices, long n) {
    btree_alloc *a = (btree_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        bt_insert_key(a, indices[i]);
    }
    return true;
}

static long btree_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double btree_aux_mem(allocator *self) {
    btree_alloc *a = (btree_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(bnode));
}

static void bt_free_nodes(bnode *node) {
    if (!node) {
        return;
    }
    if (!node->is_leaf) {
        for (int i = 0; i < node->nchild; i++) {
            bt_free_nodes(node->children[i]);
        }
    }
    free(node);
}

static void btree_destroy(allocator *self) {
    btree_alloc *a = (btree_alloc *)self;
    bt_free_nodes(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *btree_create(long size) {
    btree_alloc *a = (btree_alloc *)malloc(sizeof(btree_alloc));
    allocator_base_init(&a->base, size);
    a->root = NULL;
    a->next_fresh = 0;
    a->node_count = 0;
    a->root = bt_new(a, 1);
    a->base.allocate = btree_allocate;
    a->base.deallocate = btree_deallocate;
    a->base.count_free = btree_count_free;
    a->base.aux_mem_bytes = btree_aux_mem;
    a->base.destroy = btree_destroy;
    return &a->base;
}
