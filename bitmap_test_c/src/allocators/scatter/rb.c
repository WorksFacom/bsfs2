/*
 * Lazy allocator using a Red-Black tree of recycled blocks + next_fresh cursor.
 * Port of allocators/scatter/rbtree.py.
 */
#include <stdlib.h>

#include "allocators.h"

#define RB_RED 0
#define RB_BLACK 1

typedef struct rb_node {
    long index;
    int color;
    struct rb_node *left;
    struct rb_node *right;
    struct rb_node *parent;
} rb_node;

typedef struct {
    allocator base;
    rb_node *root;
    long next_fresh;
    long node_count;
} rb_alloc;

static int rb_get_color(rb_node *n) {
    return n ? n->color : RB_BLACK;
}

static rb_node *rb_find_min(rb_node *node) {
    if (!node) {
        return NULL;
    }
    while (node->left) {
        node = node->left;
    }
    return node;
}

static rb_node *rb_find_node(rb_node *node, long index) {
    while (node) {
        if (index == node->index) {
            return node;
        } else if (index < node->index) {
            node = node->left;
        } else {
            node = node->right;
        }
    }
    return NULL;
}

static void rb_rotate_left(rb_alloc *a, rb_node *node) {
    rb_node *rc = node->right;
    node->right = rc->left;
    if (rc->left) {
        rc->left->parent = node;
    }
    rc->parent = node->parent;
    if (!node->parent) {
        a->root = rc;
    } else if (node->parent->left == node) {
        node->parent->left = rc;
    } else {
        node->parent->right = rc;
    }
    rc->left = node;
    node->parent = rc;
}

static void rb_rotate_right(rb_alloc *a, rb_node *node) {
    rb_node *lc = node->left;
    node->left = lc->right;
    if (lc->right) {
        lc->right->parent = node;
    }
    lc->parent = node->parent;
    if (!node->parent) {
        a->root = lc;
    } else if (node->parent->left == node) {
        node->parent->left = lc;
    } else {
        node->parent->right = lc;
    }
    lc->right = node;
    node->parent = lc;
}

static void rb_fix_insert(rb_alloc *a, rb_node *node) {
    while (node->parent && node->parent->color == RB_RED) {
        if (!node->parent->parent) {
            break;
        }
        if (node->parent == node->parent->parent->left) {
            rb_node *uncle = node->parent->parent->right;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                node->parent->parent->color = RB_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    rb_rotate_left(a, node);
                }
                node->parent->color = RB_BLACK;
                node->parent->parent->color = RB_RED;
                rb_rotate_right(a, node->parent->parent);
            }
        } else {
            rb_node *uncle = node->parent->parent->left;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                node->parent->parent->color = RB_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    rb_rotate_right(a, node);
                }
                node->parent->color = RB_BLACK;
                node->parent->parent->color = RB_RED;
                rb_rotate_left(a, node->parent->parent);
            }
        }
    }
    a->root->color = RB_BLACK;
}

static void rb_insert_node(rb_alloc *a, long index) {
    rb_node *new_node = (rb_node *)malloc(sizeof(rb_node));
    new_node->index = index;
    new_node->color = RB_RED;
    new_node->left = new_node->right = new_node->parent = NULL;

    if (!a->root) {
        new_node->color = RB_BLACK;
        a->root = new_node;
        a->node_count += 1;
        return;
    }

    rb_node *parent = NULL;
    rb_node *current = a->root;
    while (current) {
        parent = current;
        if (index < current->index) {
            current = current->left;
        } else if (index > current->index) {
            current = current->right;
        } else {
            free(new_node);
            return;
        }
    }

    new_node->parent = parent;
    if (index < parent->index) {
        parent->left = new_node;
    } else {
        parent->right = new_node;
    }
    a->node_count += 1;
    rb_fix_insert(a, new_node);
}

static void rb_fix_delete(rb_alloc *a, rb_node *node) {
    while (node != a->root && rb_get_color(node) == RB_BLACK) {
        if (node == node->parent->left) {
            rb_node *sibling = node->parent->right;
            if (rb_get_color(sibling) == RB_RED) {
                sibling->color = RB_BLACK;
                node->parent->color = RB_RED;
                rb_rotate_left(a, node->parent);
                sibling = node->parent->right;
            }
            int left_color = sibling ? rb_get_color(sibling->left) : RB_BLACK;
            int right_color = sibling ? rb_get_color(sibling->right) : RB_BLACK;
            if (left_color == RB_BLACK && right_color == RB_BLACK) {
                if (sibling) {
                    sibling->color = RB_RED;
                }
                node = node->parent;
            } else {
                if (right_color == RB_BLACK) {
                    if (sibling->left) {
                        sibling->left->color = RB_BLACK;
                    }
                    sibling->color = RB_RED;
                    rb_rotate_right(a, sibling);
                    sibling = node->parent->right;
                }
                if (sibling) {
                    sibling->color = node->parent->color;
                }
                node->parent->color = RB_BLACK;
                if (sibling && sibling->right) {
                    sibling->right->color = RB_BLACK;
                }
                rb_rotate_left(a, node->parent);
                node = a->root;
            }
        } else {
            rb_node *sibling = node->parent->left;
            if (rb_get_color(sibling) == RB_RED) {
                sibling->color = RB_BLACK;
                node->parent->color = RB_RED;
                rb_rotate_right(a, node->parent);
                sibling = node->parent->left;
            }
            int left_color = sibling ? rb_get_color(sibling->left) : RB_BLACK;
            int right_color = sibling ? rb_get_color(sibling->right) : RB_BLACK;
            if (right_color == RB_BLACK && left_color == RB_BLACK) {
                if (sibling) {
                    sibling->color = RB_RED;
                }
                node = node->parent;
            } else {
                if (left_color == RB_BLACK) {
                    if (sibling->right) {
                        sibling->right->color = RB_BLACK;
                    }
                    sibling->color = RB_RED;
                    rb_rotate_left(a, sibling);
                    sibling = node->parent->left;
                }
                if (sibling) {
                    sibling->color = node->parent->color;
                }
                node->parent->color = RB_BLACK;
                if (sibling && sibling->left) {
                    sibling->left->color = RB_BLACK;
                }
                rb_rotate_right(a, node->parent);
                node = a->root;
            }
        }
    }
    /* NOTE: rbtree.py does not recolor `node` to BLACK after the loop; kept
     * faithful. Only colors are affected, never the stored index set / order. */
}

static void rb_fix_delete_null(rb_alloc *a, rb_node *parent, int is_left) {
    rb_node *node = NULL;
    (void)is_left;
    while (parent && (node == NULL || node->color == RB_BLACK)) {
        if (node == NULL || node == parent->left) {
            rb_node *sibling = parent->right;
            if (sibling && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_left(a, parent);
                sibling = parent->right;
            }
            if (sibling) {
                int left_color = rb_get_color(sibling->left);
                int right_color = rb_get_color(sibling->right);
                if (left_color == RB_BLACK && right_color == RB_BLACK) {
                    sibling->color = RB_RED;
                    node = parent;
                    parent = node->parent;
                } else {
                    if (right_color == RB_BLACK) {
                        if (sibling->left) {
                            sibling->left->color = RB_BLACK;
                        }
                        sibling->color = RB_RED;
                        rb_rotate_right(a, sibling);
                        sibling = parent->right;
                    }
                    sibling->color = parent->color;
                    parent->color = RB_BLACK;
                    if (sibling && sibling->right) {
                        sibling->right->color = RB_BLACK;
                    }
                    rb_rotate_left(a, parent);
                    break;
                }
            } else {
                node = parent;
                parent = node->parent;
            }
        } else {
            rb_node *sibling = parent->left;
            if (sibling && sibling->color == RB_RED) {
                sibling->color = RB_BLACK;
                parent->color = RB_RED;
                rb_rotate_right(a, parent);
                sibling = parent->left;
            }
            if (sibling) {
                int left_color = rb_get_color(sibling->left);
                int right_color = rb_get_color(sibling->right);
                if (right_color == RB_BLACK && left_color == RB_BLACK) {
                    sibling->color = RB_RED;
                    node = parent;
                    parent = node->parent;
                } else {
                    if (left_color == RB_BLACK) {
                        if (sibling->right) {
                            sibling->right->color = RB_BLACK;
                        }
                        sibling->color = RB_RED;
                        rb_rotate_left(a, sibling);
                        sibling = parent->left;
                    }
                    sibling->color = parent->color;
                    parent->color = RB_BLACK;
                    if (sibling && sibling->left) {
                        sibling->left->color = RB_BLACK;
                    }
                    rb_rotate_right(a, parent);
                    break;
                }
            } else {
                node = parent;
                parent = node->parent;
            }
        }
    }
}

static void rb_delete_node(rb_alloc *a, long index) {
    rb_node *node = rb_find_node(a->root, index);
    if (!node) {
        return;
    }
    if (node->left && node->right) {
        rb_node *successor = rb_find_min(node->right);
        node->index = successor->index;
        node = successor;
    }
    rb_node *child = node->left ? node->left : node->right;
    if (child) {
        child->parent = node->parent;
    }
    if (!node->parent) {
        a->root = child;
    } else {
        if (node->parent->left == node) {
            node->parent->left = child;
        } else {
            node->parent->right = child;
        }
    }
    if (node->color == RB_BLACK) {
        if (child) {
            rb_fix_delete(a, child);
        } else if (node->parent) {
            rb_fix_delete_null(a, node->parent, node == node->parent->left);
        }
    }
    free(node);
    a->node_count -= 1;
}

static long rb_select_min_free(rb_alloc *a) {
    rb_node *rmn = a->root ? rb_find_min(a->root) : NULL;
    long recycled_min = rmn ? rmn->index : -1;
    long fresh_min = (a->next_fresh < a->base.size) ? a->next_fresh : -1;
    if (recycled_min == -1) {
        return fresh_min;
    }
    if (fresh_min == -1) {
        return recycled_min;
    }
    return recycled_min < fresh_min ? recycled_min : fresh_min;
}

static long rb_allocate(allocator *self, long count, long *out) {
    rb_alloc *a = (rb_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long idx = rb_select_min_free(a);
        if (idx == -1) {
            return 0;
        }
        if (a->root && idx == rb_find_min(a->root)->index) {
            rb_delete_node(a, idx);
        } else {
            a->next_fresh += 1;
        }
        bitmap_set(&self->bm, (size_t)idx, 1);
        out[i] = idx;
    }
    return count;
}

static bool rb_deallocate(allocator *self, const long *indices, long n) {
    rb_alloc *a = (rb_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        rb_insert_node(a, indices[i]);
    }
    return true;
}

static long rb_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double rb_aux_mem(allocator *self) {
    rb_alloc *a = (rb_alloc *)self;
    return (double)((size_t)a->node_count * sizeof(rb_node));
}

static void rb_free_nodes(rb_node *node) {
    if (!node) {
        return;
    }
    rb_free_nodes(node->left);
    rb_free_nodes(node->right);
    free(node);
}

static void rb_destroy(allocator *self) {
    rb_alloc *a = (rb_alloc *)self;
    rb_free_nodes(a->root);
    allocator_base_destroy(self);
    free(a);
}

allocator *rb_create(long size) {
    rb_alloc *a = (rb_alloc *)malloc(sizeof(rb_alloc));
    allocator_base_init(&a->base, size);
    a->root = NULL;
    a->next_fresh = 0;
    a->node_count = 0;
    a->base.allocate = rb_allocate;
    a->base.deallocate = rb_deallocate;
    a->base.count_free = rb_count_free;
    a->base.aux_mem_bytes = rb_aux_mem;
    a->base.destroy = rb_destroy;
    return &a->base;
}
