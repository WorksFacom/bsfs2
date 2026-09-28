"""Scatter allocators: allocate_blocks returns the N smallest free indices, contiguous or not.

`avl_range` is range-backed but still scatters (it drains the leftmost range one block at a
time), so its bitmap matches `baseline`.
"""
