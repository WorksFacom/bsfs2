"""
Reproducible seed lists for statistical benchmarks.

`generate_seeds` draws `num_seeds` ints from `random.Random(seed_base)`, so the same
`seed_base` always gives the same list. Each statistical run gets its own seed.
"""

from __future__ import annotations

import random


def generate_seeds(num_seeds, seed_base):
    rng = random.Random(seed_base)
    return [rng.randrange(0, 2**32 - 1) for _ in range(num_seeds)]
