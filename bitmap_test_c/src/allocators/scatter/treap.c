/*
 * Lazy allocator using a treap (randomized BST) for recycled blocks.
 * Port of allocators/scatter/treap.py.
 *
 * The produced bitmap is baseline-identical regardless of priorities (selection
 * is purely by minimum index); priorities use the project PRNG for balancing.
 */
#include <stdlib.h>

#include "allocators.h"
#include "rng.h"

typedef struct treap_node {
    long idx;
    double priority;
    struct treap_node *left;
    struct treap_node *right;
} treap_node;

typedef struct {
    allocator base;
    treap_node *root;
    long next_fresh;
    long node_count;
    rng_state rng;
} treap_alloc;

static treap_node *treap_rotate_right(treap_node *node) {
    if (node->left == NULL) {
        return node;
    }
    treap_node *new_root = node->left;
    node->left = new_root->right;
    new_root->right = node;
    return new_root;
}

static treap_node *treap_rotate_left(treap_node *node) {
    if (node->right == NULL) {
        return node;
    }
    treap_node *new_root = node->right;
    node->right = new_root->left;
    new_root->left = node;
    return new_root;
}

static treap_node *treap_insert(treap_node *node, treap_node *new_node) {
    if (node == NULL) {
        return new_node;
    }
    if (new_node->idx < node->idx) {
        node->left = treap_insert(node->left, new_node);
        if (node->left->priority > node->priority) {
            node = treap_rotate_right(node);
        }
    } else {
        node->right = treap_insert(node->right, new_node);
        if (node->right->priority > node->priority) {
            node = treap_rotate_left(node);
        }
    }
    return node;
}

static treap_node *treap_find_min(treap_node *node) {
    if (node == NULL) {
        return NULL;
    }
    while (node->left != NULL) {
        node = node->left;
    }
    return node;
}

static treap_node *treap_delete(treap_node *node, long idx) {
    if (node == NULL) {
        return NULL;
    }
    if (idx < node->idx) {
        node->left = treap_delete(node->left, idx);
    } else if (idx > node->idx) {
        node->right = treap_delete(node->right, idx);
    } else {
        if (node->left == NULL) {
            treap_node *r = node->right;
            free(node);
            return r;
        } else if (node->right == NULL) {
            treap_node *l = node->left;
            free(node);
            return l;
        }
        if (node->left->priority > node->right->priority) {
            node = treap_rotate_right(node);
            node->right = treap_delete(node->right, idx);
        } else {
            node = treap_rotate_left(node);
            node->left = treap_delete(node->left, idx);
        }
    }
    return node;
}

static long treap_select_min_free(treap_alloc *a) {
    treap_node *min_node = treap_find_min(a->root);
    long treap_min = min_node ? min_node->idx : -1;
    long fresh_min = (a->next_fresh < a->base.size) ? a->next_fresh : -1;
    if (treap_min < 0) {
        return fresh_min;
    }
    if (fresh_min < 0) {
        return treap_min;
    }
    return treap_min < fresh_min ? treap_min : fresh_min;
}

static long treap_allocate(allocator *self, long count, long *out) {
    treap_alloc *a = (treap_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long min_idx = treap_select_min_free(a);
        if (min_idx < 0) {
            return 0;
        }
        bitmap_set(&self->bm, (size_t)min_idx, 1);
        out[i] = min_idx;
        if (min_idx == a->next_fresh) {
            a->next_fresh += 1;
        } else {
            a->root = treap_delete(a->root, min_idx);
            a->node_count -= 1;
        }
    }
    return count;
}

static bool treap_deallocate(allocator *self, const long *indices, long n) {
    treap_alloc *a = (treap_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        treap_node *node = (treap_node *)malloc(sizeof(treap_node));
        node->idx = indices[i];
        node->priority = rng_double(&a->rng);
        node->left = NULL;
        node->right = NULL;
        a->root = treap_insert(a->root, node);
        a->node_count += 1;
    }
    return true;
}

static long treap_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double treap_aux_mem(allocator *self) {
    treap_alloc *a = (treap_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(treap_node));
}

static void treap_free_nodes(treap_node *node) {
    if (node == NULL) {
        return;
    }
    treap_free_nodes(node->left);
    treap_free_nodes(node->right);
    free(node);
}

static void treap_destroy(allocator *self) {
    treap_alloc *a = (treap_alloc *)self;
    treap_free_nodes(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *treap_create(long size) {
    treap_alloc *a = (treap_alloc *)malloc(sizeof(treap_alloc));
    allocator_base_init(&a->base, size);
    a->root = NULL;
    a->next_fresh = 0;
    a->node_count = 0;
    rng_seed(&a->rng, 0x5eed5eed);
    a->base.allocate = treap_allocate;
    a->base.deallocate = treap_deallocate;
    a->base.count_free = treap_count_free;
    a->base.aux_mem_bytes = treap_aux_mem;
    a->base.destroy = treap_destroy;
    return &a->base;
}
