"""Statistical aggregation helpers for repeated benchmark runs."""

from __future__ import annotations

import random


def generate_seeds(num_seeds, seed_base):
    rng = random.Random(seed_base)
    return [rng.randrange(0, 2**32 - 1) for _ in range(num_seeds)]
