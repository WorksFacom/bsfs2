/*
 * deterministic PRNG (SplitMix64) standing in for Python's random.Random.
 *
 * NOT CPython's Mersenne Twister, so absolute values differ from the Python suite. what carries over
 * is the workload structure (thresholds, sampling without replacement, per-iteration call order), so
 * on a given seed every strategy still sees the same workload and comparisons hold.
 */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct {
    uint64_t s;
} rng_state;

void rng_seed(rng_state *st, uint64_t seed);

uint64_t rng_next_u64(rng_state *st);

/* random(): double in [0, 1) */
double rng_double(rng_state *st);

/* randrange(0, n) / _randbelow(n): unbiased int in [0, n) */
long rng_below(rng_state *st, long n);

/* sample(population, k): k distinct elements of population[0..n) into out, in no particular order */
void rng_sample(rng_state *st, const long *population, long n, long k, long *out);

/* sample(range(n), k): k distinct ints from [0, n) into out, in no particular order */
void rng_sample_range(rng_state *st, long n, long k, long *out);

#endif /* RNG_H */
