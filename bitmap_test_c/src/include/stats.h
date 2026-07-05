/*
 * Reproducible seed-list generation. Port of core/stats.py::generate_seeds.
 * Uses the project PRNG (not CPython's MT) seeded with seed_base.
 */
#ifndef STATS_H
#define STATS_H

/* Returns a malloc'd array of `num_seeds` seeds derived from seed_base.
 * Caller frees. */
long *generate_seeds(long num_seeds, long seed_base);

#endif /* STATS_H */
