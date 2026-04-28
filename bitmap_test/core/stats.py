"""
Helpers for generating reproducible seed lists for statistical benchmarks.

`generate_seeds`:
- produces `num_seeds` integer seeds from a `seed_base` via `random.Random`,
  guaranteeing that the same seed list is reproduced for the same `seed_base`.
- each statistical run uses a different seed to sample workload variance across the strategy.
"""

from __future__ import annotations

import random


def generate_seeds(num_seeds, seed_base):
    rng = random.Random(seed_base)
    return [rng.randrange(0, 2**32 - 1) for _ in range(num_seeds)]
