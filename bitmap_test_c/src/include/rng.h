/*
 * Deterministic PRNG used in place of Python's random.Random.
 *
 * This does NOT reproduce CPython's Mersenne Twister: absolute values differ
 * from the Python suite. What is preserved is the *structure* of the workloads
 * (thresholds, sampling-without-replacement, and the per-iteration order of
 * calls), so that on a given seed every strategy still sees the same workload
 * and cross-strategy comparison stays valid.
 *
 * Backed by SplitMix64.
 */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct {
    uint64_t s;
} rng_state;

void rng_seed(rng_state *st, uint64_t seed);

/* Raw 64-bit draw (SplitMix64). */
uint64_t rng_next_u64(rng_state *st);

/* random() equivalent: a double in [0, 1). */
double rng_double(rng_state *st);

/* randrange(0, n) / _randbelow(n): unbiased integer in [0, n). */
long rng_below(rng_state *st, long n);

/* sample(population, k): write k distinct elements of population[0..n) to out.
 * Output order is unspecified (consumers treat it as a set). */
void rng_sample(rng_state *st, const long *population, long n, long k, long *out);

/* sample(range(n), k): write k distinct integers from [0, n) to out.
 * Output order is unspecified. */
void rng_sample_range(rng_state *st, long n, long k, long *out);

#endif /* RNG_H */
