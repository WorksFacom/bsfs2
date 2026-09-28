/*
 * one `*_create(long size)` factory per allocator, each returning an `allocator *`.
 */
#ifndef ALLOCATORS_H
#define ALLOCATORS_H

#include "base.h"

/* scatter */
allocator *baseline_create(long size);
allocator *needle_create(long size);
allocator *avl_create(long size);
allocator *avl_range_create(long size);
allocator *rb_create(long size);
allocator *heap_create(long size);
allocator *btree_create(long size);
allocator *bplustree_create(long size);
allocator *hybrid_create(long size);
allocator *segtree_create(long size);
allocator *twolevel_create(long size);
allocator *treap_create(long size);

/* extent */
allocator *avl_range_extent_create(long size);
allocator *treap_range_extent_create(long size);
allocator *rb_range_extent_create(long size);
allocator *bplustree_extent_create(long size);

#endif /* ALLOCATORS_H */
