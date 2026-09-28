/*
 * hybrid heap + RB tree allocator. port of allocators/scatter/hybrid_heap_rb.py.
 *
 * allocation uses an indexed min-heap (heap[] + position_map). the RB tree never affects the bitmap
 * (selection is heap top vs next_fresh), it's kept in sync so timing and memory match Python.
 */
#include <stdlib.h>

#include "allocators.h"
#include "longmap.h"

#define HB_RED 0
#define HB_BLACK 1

typedef struct hb_rbnode {
    long key;
    int color;
    struct hb_rbnode *left;
    struct hb_rbnode *right;
    struct hb_rbnode *parent;
} hb_rbnode;

typedef struct {
    hb_rbnode *root;
    long node_count;
} hb_rbtree;

typedef struct {
    allocator base;
    long *heap;
    long heap_len;
    long heap_cap;
    longmap position_map;
    long next_fresh;
    hb_rbtree rb;
} hybrid_alloc;

/* ---------- RB tree, dealloc tracking ---------- */
static int hb_color(hb_rbnode *n) {
    return n ? n->color : HB_BLACK;
}

static hb_rbnode *hb_min_node(hb_rbnode *n) {
    while (n->left) {
        n = n->left;
    }
    return n;
}

static hb_rbnode *hb_find(hb_rbnode *n, long key) {
    while (n) {
        if (key == n->key) {
            return n;
        } else if (key < n->key) {
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return NULL;
}

static void hb_rotate_left(hb_rbtree *t, hb_rbnode *node) {
    hb_rbnode *rc = node->right;
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
}

static void hb_rotate_right(hb_rbtree *t, hb_rbnode *node) {
    hb_rbnode *lc = node->left;
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
}

static void hb_fix_insert(hb_rbtree *t, hb_rbnode *node) {
    while (node->parent && node->parent->color == HB_RED) {
        if (!node->parent->parent) {
            break;
        }
        if (node->parent == node->parent->parent->left) {
            hb_rbnode *uncle = node->parent->parent->right;
            if (uncle && uncle->color == HB_RED) {
                node->parent->color = HB_BLACK;
                uncle->color = HB_BLACK;
                node->parent->parent->color = HB_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    hb_rotate_left(t, node);
                }
                node->parent->color = HB_BLACK;
                node->parent->parent->color = HB_RED;
                hb_rotate_right(t, node->parent->parent);
            }
        } else {
            hb_rbnode *uncle = node->parent->parent->left;
            if (uncle && uncle->color == HB_RED) {
                node->parent->color = HB_BLACK;
                uncle->color = HB_BLACK;
                node->parent->parent->color = HB_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    hb_rotate_right(t, node);
                }
                node->parent->color = HB_BLACK;
                node->parent->parent->color = HB_RED;
                hb_rotate_left(t, node->parent->parent);
            }
        }
    }
    t->root->color = HB_BLACK;
}

static void hb_rb_insert(hb_rbtree *t, long key) {
    hb_rbnode *new_node = (hb_rbnode *)malloc(sizeof(hb_rbnode));
    new_node->key = key;
    new_node->color = HB_RED;
    new_node->left = new_node->right = new_node->parent = NULL;

    if (!t->root) {
        new_node->color = HB_BLACK;
        t->root = new_node;
        t->node_count += 1;
        return;
    }
    hb_rbnode *parent = NULL;
    hb_rbnode *current = t->root;
    while (current) {
        parent = current;
        if (key < current->key) {
            current = current->left;
        } else if (key > current->key) {
            current = current->right;
        } else {
            free(new_node);
            return;
        }
    }
    new_node->parent = parent;
    if (key < parent->key) {
        parent->left = new_node;
    } else {
        parent->right = new_node;
    }
    t->node_count += 1;
    hb_fix_insert(t, new_node);
}

static void hb_fix_delete(hb_rbtree *t, hb_rbnode *node) {
    while (node != t->root && hb_color(node) == HB_BLACK) {
        if (node == node->parent->left) {
            hb_rbnode *sibling = node->parent->right;
            if (hb_color(sibling) == HB_RED) {
                sibling->color = HB_BLACK;
                node->parent->color = HB_RED;
                hb_rotate_left(t, node->parent);
                sibling = node->parent->right;
            }
            int lc = sibling ? hb_color(sibling->left) : HB_BLACK;
            int rc = sibling ? hb_color(sibling->right) : HB_BLACK;
            if (lc == HB_BLACK && rc == HB_BLACK) {
                if (sibling) {
                    sibling->color = HB_RED;
                }
                node = node->parent;
            } else {
                if (rc == HB_BLACK) {
                    if (sibling->left) {
                        sibling->left->color = HB_BLACK;
                    }
                    sibling->color = HB_RED;
                    hb_rotate_right(t, sibling);
                    sibling = node->parent->right;
                }
                if (sibling) {
                    sibling->color = node->parent->color;
                }
                node->parent->color = HB_BLACK;
                if (sibling && sibling->right) {
                    sibling->right->color = HB_BLACK;
                }
                hb_rotate_left(t, node->parent);
                node = t->root;
            }
        } else {
            hb_rbnode *sibling = node->parent->left;
            if (hb_color(sibling) == HB_RED) {
                sibling->color = HB_BLACK;
                node->parent->color = HB_RED;
                hb_rotate_right(t, node->parent);
                sibling = node->parent->left;
            }
            int lc = sibling ? hb_color(sibling->left) : HB_BLACK;
            int rc = sibling ? hb_color(sibling->right) : HB_BLACK;
            if (rc == HB_BLACK && lc == HB_BLACK) {
                if (sibling) {
                    sibling->color = HB_RED;
                }
                node = node->parent;
            } else {
                if (lc == HB_BLACK) {
                    if (sibling->right) {
                        sibling->right->color = HB_BLACK;
                    }
                    sibling->color = HB_RED;
                    hb_rotate_left(t, sibling);
                    sibling = node->parent->left;
                }
                if (sibling) {
                    sibling->color = node->parent->color;
                }
                node->parent->color = HB_BLACK;
                if (sibling && sibling->left) {
                    sibling->left->color = HB_BLACK;
                }
                hb_rotate_right(t, node->parent);
                node = t->root;
            }
        }
    }
}

static void hb_fix_delete_null(hb_rbtree *t, hb_rbnode *parent, int is_left) {
    hb_rbnode *node = NULL;
    (void)is_left;
    while (parent && (node == NULL || node->color == HB_BLACK)) {
        if (node == NULL || node == parent->left) {
            hb_rbnode *sibling = parent->right;
            if (sibling && sibling->color == HB_RED) {
                sibling->color = HB_BLACK;
                parent->color = HB_RED;
                hb_rotate_left(t, parent);
                sibling = parent->right;
            }
            if (sibling) {
                int lc = hb_color(sibling->left);
                int rc = hb_color(sibling->right);
                if (lc == HB_BLACK && rc == HB_BLACK) {
                    sibling->color = HB_RED;
                    node = parent;
                    parent = node->parent;
                } else {
                    if (rc == HB_BLACK) {
                        if (sibling->left) {
                            sibling->left->color = HB_BLACK;
                        }
                        sibling->color = HB_RED;
                        hb_rotate_right(t, sibling);
                        sibling = parent->right;
                    }
                    sibling->color = parent->color;
                    parent->color = HB_BLACK;
                    if (sibling && sibling->right) {
                        sibling->right->color = HB_BLACK;
                    }
                    hb_rotate_left(t, parent);
                    break;
                }
            } else {
                node = parent;
                parent = node->parent;
            }
        } else {
            hb_rbnode *sibling = parent->left;
            if (sibling && sibling->color == HB_RED) {
                sibling->color = HB_BLACK;
                parent->color = HB_RED;
                hb_rotate_right(t, parent);
                sibling = parent->left;
            }
            if (sibling) {
                int lc = hb_color(sibling->left);
                int rc = hb_color(sibling->right);
                if (rc == HB_BLACK && lc == HB_BLACK) {
                    sibling->color = HB_RED;
                    node = parent;
                    parent = node->parent;
                } else {
                    if (lc == HB_BLACK) {
                        if (sibling->right) {
                            sibling->right->color = HB_BLACK;
                        }
                        sibling->color = HB_RED;
                        hb_rotate_left(t, sibling);
                        sibling = parent->left;
                    }
                    sibling->color = parent->color;
                    parent->color = HB_BLACK;
                    if (sibling && sibling->left) {
                        sibling->left->color = HB_BLACK;
                    }
                    hb_rotate_right(t, parent);
                    break;
                }
            } else {
                node = parent;
                parent = node->parent;
            }
        }
    }
}

static void hb_rb_delete(hb_rbtree *t, long key) {
    hb_rbnode *node = hb_find(t->root, key);
    if (!node) {
        return;
    }
    if (node->left && node->right) {
        hb_rbnode *successor = hb_min_node(node->right);
        node->key = successor->key;
        node = successor;
    }
    hb_rbnode *child = node->left ? node->left : node->right;
    if (child) {
        child->parent = node->parent;
    }
    if (!node->parent) {
        t->root = child;
    } else {
        if (node->parent->left == node) {
            node->parent->left = child;
        } else {
            node->parent->right = child;
        }
    }
    if (node->color == HB_BLACK) {
        if (child) {
            hb_fix_delete(t, child);
        } else if (node->parent) {
            hb_fix_delete_null(t, node->parent, node == node->parent->left);
        }
    }
    free(node);
    t->node_count -= 1;
}

static void hb_rb_free(hb_rbnode *n) {
    if (!n) {
        return;
    }
    hb_rb_free(n->left);
    hb_rb_free(n->right);
    free(n);
}

/* ---------- indexed min-heap ---------- */
static void hb_sift_up(hybrid_alloc *a, long pos) {
    long key = a->heap[pos];
    while (pos > 0) {
        long parent_pos = (pos - 1) / 2;
        long parent_key = a->heap[parent_pos];
        if (key < parent_key) {
            a->heap[pos] = parent_key;
            lm_set(&a->position_map, parent_key, pos);
            pos = parent_pos;
        } else {
            break;
        }
    }
    a->heap[pos] = key;
    lm_set(&a->position_map, key, pos);
}

/*
 * deliberately differs from hybrid_heap_rb.py. the Python _sift_down compares children against
 * `heap[smallest]`, which goes stale after the first level, so it can break the heap property and
 * return a non-minimal root. comparing against the carried `key` is the min-heap that was meant,
 * and keeps `hybrid` matching baseline under any RNG.
 */
static void hb_sift_down(hybrid_alloc *a, long pos) {
    long key = a->heap[pos];
    long size = a->heap_len;
    for (;;) {
        long smallest = pos;
        long best = key;
        long left = 2 * pos + 1;
        long right = 2 * pos + 2;
        if (left < size && a->heap[left] < best) {
            smallest = left;
            best = a->heap[left];
        }
        if (right < size && a->heap[right] < best) {
            smallest = right;
            best = a->heap[right];
        }
        if (smallest != pos) {
            a->heap[pos] = a->heap[smallest];
            lm_set(&a->position_map, a->heap[pos], pos);
            pos = smallest;
        } else {
            break;
        }
    }
    a->heap[pos] = key;
    lm_set(&a->position_map, key, pos);
}

static void hb_insert_key(hybrid_alloc *a, long key) {
    hb_rb_insert(&a->rb, key);
    if (!lm_contains(&a->position_map, key)) {
        if (a->heap_len == a->heap_cap) {
            a->heap_cap = a->heap_cap ? a->heap_cap * 2 : 16;
            a->heap = (long *)realloc(a->heap, (size_t)a->heap_cap * sizeof(long));
        }
        long pos = a->heap_len++;
        a->heap[pos] = key;
        lm_set(&a->position_map, key, pos);
        hb_sift_up(a, pos);
    }
}

static void hb_delete_key(hybrid_alloc *a, long key) {
    hb_rb_delete(&a->rb, key);
    if (lm_contains(&a->position_map, key)) {
        long pos = lm_get(&a->position_map, key, 0);
        long last_key = a->heap[a->heap_len - 1];
        if (key != last_key) {
            a->heap[pos] = last_key;
            lm_set(&a->position_map, last_key, pos);
            if (pos > 0 && a->heap[pos] < a->heap[(pos - 1) / 2]) {
                hb_sift_up(a, pos);
            } else {
                hb_sift_down(a, pos);
            }
        }
        a->heap_len -= 1;
        lm_del(&a->position_map, key);
    }
}

static long hb_find_free_block(hybrid_alloc *a) {
    long recycled_min = a->heap_len > 0 ? a->heap[0] : -1;
    long fresh_min = (a->next_fresh < a->base.size) ? a->next_fresh : -1;
    if (recycled_min == -1) {
        return fresh_min;
    }
    if (fresh_min == -1) {
        return recycled_min;
    }
    return recycled_min < fresh_min ? recycled_min : fresh_min;
}

static long hybrid_allocate(allocator *self, long count, long *out) {
    hybrid_alloc *a = (hybrid_alloc *)self;
    if (count <= 0) {
        return 0;
    }
    if (allocator_free_remaining(self) < count) {
        return 0;
    }
    for (long i = 0; i < count; i++) {
        long idx = hb_find_free_block(a);
        if (idx == -1) {
            return 0;
        }
        if (a->heap_len > 0 && idx == a->heap[0]) {
            hb_delete_key(a, idx);
        } else if (idx == a->next_fresh) {
            a->next_fresh += 1;
        }
        bitmap_set(&self->bm, (size_t)idx, 1);
        out[i] = idx;
    }
    return count;
}

static bool hybrid_deallocate(allocator *self, const long *indices, long n) {
    hybrid_alloc *a = (hybrid_alloc *)self;
    if (!allocator_validate_deallocation(self, indices, n)) {
        return false;
    }
    for (long i = 0; i < n; i++) {
        bitmap_set(&self->bm, (size_t)indices[i], 0);
        hb_insert_key(a, indices[i]);
    }
    return true;
}

static long hybrid_count_free(allocator *self) {
    return allocator_free_remaining(self);
}

static double hybrid_aux_mem(allocator *self) {
    hybrid_alloc *a = (hybrid_alloc *)self;
    double bytes = (double)((size_t)a->heap_len * sizeof(long));
    bytes += (double)((size_t)a->position_map.count * sizeof(lm_entry));
    bytes += (double)((size_t)a->rb.node_count * sizeof(hb_rbnode));
    return bytes;
}

static void hybrid_destroy(allocator *self) {
    hybrid_alloc *a = (hybrid_alloc *)self;
    free(a->heap);
    lm_free(&a->position_map);
    hb_rb_free(a->rb.root);
    allocator_base_destroy(self);
    free(a);
}

allocator *hybrid_create(long size) {
    hybrid_alloc *a = (hybrid_alloc *)malloc(sizeof(hybrid_alloc));
    allocator_base_init(&a->base, size);
    a->heap = NULL;
    a->heap_len = 0;
    a->heap_cap = 0;
    lm_init(&a->position_map);
    a->next_fresh = 0;
    a->rb.root = NULL;
    a->rb.node_count = 0;
    a->base.allocate = hybrid_allocate;
    a->base.deallocate = hybrid_deallocate;
    a->base.count_free = hybrid_count_free;
    a->base.aux_mem_bytes = hybrid_aux_mem;
    a->base.destroy = hybrid_destroy;
    return &a->base;
}
