"""Scatter allocators: allocate_blocks returns the N smallest free indices,
contiguous or not. They make no attempt to keep a request in one extent.

Note: `avl_range` is range-backed but still scatter-behaving (it drains the
leftmost range one block at a time), so its bitmap matches `baseline`.
"""
