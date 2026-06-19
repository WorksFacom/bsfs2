"""Extent allocators: allocate_blocks returns a single contiguous run when one
exists, and only scatters as a fallback. Free space is modeled as disjoint
ranges so contiguity can be searched directly.
"""
