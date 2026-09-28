/*
 * lazy allocator, AVL tree of recycled blocks + next_fresh cursor. port of allocators/scatter/avl.py.
 */
#include <stdlib.h>

#include "allocators.h"

typedef struct avl_node {
    long index;
    struct avl_node *left;
    struct avl_node *right;
    int height;
} avl_node;

typedef struct {
    allocator base;
    avl_node *root;
    long next_fresh;
    long node_count;
} avl_alloc;

static int avl_h(avl_node *n) {
    return n ? n->height : 0;
}

static int imax(int a, int b) {
    return a > b ? a : b;
}

static int avl_balance(avl_node *n) {
    return n ? avl_h(n->left) - avl_h(n->right) : 0;
}

static avl_node *avl_rotate_left(avl_node *node) {
    avl_node *rc = node->right;
    node->right = rc->left;
    rc->left = node;
    node->height = 1 + imax(avl_h(node->left), avl_h(node->right));
    rc->height = 1 + imax(avl_h(rc->left), avl_h(rc->right));
    return rc;
}

static avl_node *avl_rotate_right(avl_node *node) {
    avl_node *lc = node->left;
    node->left = lc->right;
    lc->right = node;
    node->height = 1 + imax(avl_h(node->left), avl_h(node->right));
    lc->height = 1 + imax(avl_h(lc->left), avl_h(lc->right));
    return lc;
}

static avl_node *avl_insert(avl_alloc *a, avl_node *node, long index) {
    if (!node) {
        avl_node *n = (avl_node *)malloc(sizeof(avl_node));
        n->index = index;
        n->left = n->right = NULL;
        n->height = 1;
        a->node_count += 1;
        return n;
    }
    if (index < node->index) {
        node->left = avl_insert(a, node->left, index);
    } else {
        node->right = avl_insert(a, node->right, index);
    }
    node->height = 1 + imax(avl_h(node->left), avl_h(node->right));
    int balance = avl_balance(node);

    if (balance > 1 && index < node->left->index) {
        return avl_rotate_right(node);
    }
    if (balance < -1 && index > node->right->index) {
        return avl_rotate_left(node);
    }
    if (balance > 1 && index > node->left->index) {
        node->left = avl_rotate_left(node->left);
        return avl_rotate_right(node);
    }
    if (balance < -1 && index < node->right->index) {
        node->right = avl_rotate_right(node->right);
        return avl_rotate_left(node);
    }
    return node;
}

static avl_node *avl_delete(avl_alloc *a, avl_node *node, long index) {
    if (!node) {
        return NULL;
    }
    if (index < node->index) {
        node->left = avl_delete(a, node->left, index);
    } else if (index > node->index) {
        node->right = avl_delete(a, node->right, index);
    } else {
        if (!node->left) {
            avl_node *r = node->right;
            free(node);
            a->node_count -= 1;
            return r;
        } else if (!node->right) {
            avl_node *l = node->left;
            free(node);
            a->node_count -= 1;
            return l;
        }
        avl_node *min_node = node->right;
        while (min_node->left) {
            min_node = min_node->left;
        }
        node->index = min_node->index;
        node->right = avl_delete(a, node->right, min_node->index);
    }

    node->height = 1 + imax(avl_h(node->left), avl_h(node->right));
    int balance = avl_balance(node);

    if (balance > 1 && avl_balance(node->left) >= 0) {
        return avl_rotate_right(node);
    }
    if (balance > 1 && avl_balance(node->left) < 0) {
        node->left = avl_rotate_left(node->left);
        return avl_rotate_right(node);
    }
    if (balance < -1 && avl_balance(node->right) <= 0) {
        return avl_rotate_left(node);
    }
    if (balance < -1 && avl_balance(node->right) > 0) {
        node->right = avl_rotate_right(node->right);
        return avl_rotate_left(node);
    }
    return node;
}

static long avl_find_free_block(avl_alloc *a) {
    long tree_min = -1;
    if (a->root) {
        avl_node *node = a->root;
        while (node->left) {
            node = node->left;
        }
        tree_min = node->index;
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

static long avl_allocate(allocator *self, long count, long *out) {
    avl_alloc *a = (avl_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long idx = avl_find_free_block(a);
        if (idx == -1) {
            return 0;
        }
        bitmap_set(&self->bm, (size_t)idx, 1);
        if (idx == a->next_fresh) {
            a->next_fresh += 1;
        } else {
            a->root = avl_delete(a, a->root, idx);
        }
        out[i] = idx;
    }
    return count;
}

static bool avl_deallocate(allocator *self, const long *indices, long n) {
    avl_alloc *a = (avl_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        a->root = avl_insert(a, a->root, indices[i]);
    }
    return true;
}

static long avl_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double avl_aux_mem(allocator *self) {
    avl_alloc *a = (avl_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(avl_node));
}

static void avl_free_nodes(avl_node *node) {
    if (!node) {
        return;
    }
    avl_free_nodes(node->left);
    avl_free_nodes(node->right);
    free(node);
}

static void avl_destroy(allocator *self) {
    avl_alloc *a = (avl_alloc *)self;
    avl_free_nodes(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *avl_create(long size) {
    avl_alloc *a = (avl_alloc *)malloc(sizeof(avl_alloc));
    allocator_base_init(&a->base, size);
    a->root = NULL;
    a->next_fresh = 0;
    a->node_count = 0;
    a->base.allocate = avl_allocate;
    a->base.deallocate = avl_deallocate;
    a->base.count_free = avl_count_free;
    a->base.aux_mem_bytes = avl_aux_mem;
    a->base.destroy = avl_destroy;
    return &a->base;
}
