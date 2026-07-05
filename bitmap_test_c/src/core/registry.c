#include "registry.h"

#include <string.h>

#include "allocators.h"

/* preferred_order from allocators/__init__.py. */
static const strategy_spec SPECS[] = {
    /* scatter */
    {"baseline", baseline_create, "Baseline Sequential Scan"},
    {"needle", needle_create, "Sequential Needle"},
    {"avl", avl_create, "AVL Tree"},
    {"avl_range", avl_range_create, "AVL Range Tree"},
    {"rb", rb_create, "Red-Black Tree"},
    {"heap", heap_create, "Min-Heap"},
    {"btree", btree_create, "B-Tree"},
    {"bplustree", bplustree_create, "B+Tree"},
    {"hybrid", hybrid_create, "Hybrid Heap+RB"},
    {"segtree", segtree_create, "Segment Tree"},
    {"twolevel", twolevel_create, "Two-Level Index"},
    {"treap", treap_create, "Treap"},
    /* extent */
    {"avl_range_extent", avl_range_extent_create, "AVL Range Extent-First"},
    {"treap_range_extent", treap_range_extent_create, "Treap Range Extent-First"},
    {"rb_range_extent", rb_range_extent_create, "Red-Black Range Extent-First"},
    {"bplustree_extent", bplustree_extent_create, "B+Tree Extent-First"},
};

static const long NSPECS = (long)(sizeof(SPECS) / sizeof(SPECS[0]));

const strategy_spec *registry_specs(long *count) {
    if (count) {
        *count = NSPECS;
    }
    return SPECS;
}

const strategy_spec *registry_get(const char *key) {
    for (long i = 0; i < NSPECS; i++) {
        if (strcmp(SPECS[i].key, key) == 0) {
            return &SPECS[i];
        }
    }
    return NULL;
}
