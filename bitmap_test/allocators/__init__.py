"""
Bitmap Allocators Package - Multiple strategies for bitmap block allocation.

Allocators are split into two families:
- `scatter/`: allocate_blocks returns the N smallest free indices (no contiguity).
- `extent/`: allocate_blocks returns a single contiguous run when possible,
  scattering only as a fallback.
"""

from core.registry import list_strategies

# Scatter family
from .scatter.avl import BitmapAVL
from .scatter.avl_range import BitmapAVLRange
from .scatter.rbtree import BitmapRB
from .scatter.heap import BitmapHeap
from .scatter.btree import BitmapBTree
from .scatter.bplustree import BitmapBPlusTree
from .scatter.hybrid_heap_rb import BitmapHybridHeapRB
from .scatter.segment_tree import BitmapSegmentTree
from .scatter.two_level_index import BitmapTwoLevelIndex
from .scatter.treap import BitmapTreap
from .scatter.baseline import BitmapBaseline
from .scatter.needle_sequential import BitmapNeedleSequential

# Extent family
from .extent.avl_range_extent import BitmapAVLRangeExtent
from .extent.treap_range_extent import BitmapTreapRangeExtent
from .extent.rb_range_extent import BitmapRBRangeExtent
from .extent.bplustree_extent import BitmapBPlusTreeExtent


def get_strategy_specs():
    by_key = {
        spec.key: (spec.key, spec.allocator_class, spec.display_name)
        for spec in list_strategies()
    }
    preferred_order = [
        # scatter
        "baseline",
        "needle",
        "avl",
        "avl_range",
        "rb",
        "heap",
        "btree",
        "bplustree",
        "hybrid",
        "segtree",
        "twolevel",
        "treap",
        # extent
        "avl_range_extent",
        "treap_range_extent",
        "rb_range_extent",
        "bplustree_extent",
    ]

    ordered = [by_key[key] for key in preferred_order if key in by_key]
    extras = [by_key[key] for key in sorted(by_key.keys()) if key not in preferred_order]
    return ordered + extras


__all__ = [
    'get_strategy_specs',
    'BitmapAVL',
    'BitmapAVLRange',
    'BitmapRB',
    'BitmapHeap',
    'BitmapBTree',
    'BitmapBPlusTree',
    'BitmapHybridHeapRB',
    'BitmapSegmentTree',
    'BitmapTwoLevelIndex',
    'BitmapTreap',
    'BitmapBaseline',
    'BitmapNeedleSequential',
    'BitmapAVLRangeExtent',
    'BitmapTreapRangeExtent',
    'BitmapRBRangeExtent',
    'BitmapBPlusTreeExtent',
]
