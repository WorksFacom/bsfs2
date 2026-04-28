"""
Bitmap Allocators Package - Multiple strategies for bitmap block allocation
"""

from core.registry import list_strategies

from .avl import BitmapAVL
from .rbtree import BitmapRB
from .heap import BitmapHeap
from .btree import BitmapBTree
from .bplustree import BitmapBPlusTree
from .hybrid_heap_rb import BitmapHybridHeapRB
from .segment_tree import BitmapSegmentTree
from .two_level_index import BitmapTwoLevelIndex
from .treap import BitmapTreap
from .baseline import BitmapBaseline
from .needle_sequential import BitmapNeedleSequential


def get_strategy_specs():
    by_key = {
        spec.key: (spec.key, spec.allocator_class, spec.display_name)
        for spec in list_strategies()
    }
    preferred_order = [
        "baseline",
        "needle",
        "avl",
        "rb",
        "heap",
        "btree",
        "bplustree",
        "hybrid",
        "segtree",
        "twolevel",
        "treap",
    ]

    ordered = [by_key[key] for key in preferred_order if key in by_key]
    extras = [by_key[key] for key in sorted(by_key.keys()) if key not in preferred_order]
    return ordered + extras


__all__ = [
    'get_strategy_specs',
    'BitmapAVL',
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
]
