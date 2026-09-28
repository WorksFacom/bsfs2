/*
 * reproducible seed lists, port of core/stats.py::generate_seeds. uses the project PRNG, not CPython's MT.
 */
#ifndef STATS_H
#define STATS_H

/* malloc'd, caller frees */
long *generate_seeds(long num_seeds, long seed_base);

#endif /* STATS_H */
