#include "rng.h"

#include <stdlib.h>

void rng_seed(rng_state *st, uint64_t seed) {
    st->s = seed;
}

uint64_t rng_next_u64(rng_state *st) {
    /* SplitMix64 */
    uint64_t z = (st->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double rng_double(rng_state *st) {
    /* top 53 bits -> [0, 1) */
    uint64_t x = rng_next_u64(st) >> 11;
    return (double)x * (1.0 / 9007199254740992.0);
}

long rng_below(rng_state *st, long n) {
    if (n <= 0) {
        return 0;
    }
    uint64_t un = (uint64_t)n;
    /* rejection sampling, unbiased */
    uint64_t limit = UINT64_MAX - (UINT64_MAX % un);
    uint64_t r;
    do {
        r = rng_next_u64(st);
    } while (r >= limit);
    return (long)(r % un);
}

/* --- small open-addressing int set for the "set method" --- */
typedef struct {
    long *slots;
    long cap;
} long_set;

static long next_pow2(long x) {
    long p = 1;
    while (p < x) {
        p <<= 1;
    }
    return p;
}

static void long_set_init(long_set *s, long k) {
    s->cap = next_pow2(k * 2 + 1);
    s->slots = (long *)malloc((size_t)s->cap * sizeof(long));
    for (long i = 0; i < s->cap; i++) {
        s->slots[i] = -1;
    }
}

static int long_set_add(long_set *s, long key) {
    uint64_t mask = (uint64_t)s->cap - 1;
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ULL;
    long pos = (long)(h & mask);
    while (s->slots[pos] != -1) {
        if (s->slots[pos] == key) {
            return 0; /* already present */
        }
        pos = (long)((pos + 1) & mask);
    }
    s->slots[pos] = key;
    return 1;
}

static void long_set_free(long_set *s) {
    free(s->slots);
}

void rng_sample(rng_state *st, const long *population, long n, long k, long *out) {
    if (k <= 0) {
        return;
    }
    if (k > n) {
        k = n;
    }

    /* pool method when k is a big fraction of n, set method otherwise, same split as CPython */
    if (k > n / 2) {
        long *pool = (long *)malloc((size_t)n * sizeof(long));
        for (long i = 0; i < n; i++) {
            pool[i] = population[i];
        }
        for (long i = 0; i < k; i++) {
            long j = i + rng_below(st, n - i);
            long tmp = pool[i];
            pool[i] = pool[j];
            pool[j] = tmp;
            out[i] = pool[i];
        }
        free(pool);
    } else {
        long_set selected;
        long_set_init(&selected, k);
        long count = 0;
        while (count < k) {
            long j = rng_below(st, n);
            if (long_set_add(&selected, j)) {
                out[count++] = population[j];
            }
        }
        long_set_free(&selected);
    }
}

void rng_sample_range(rng_state *st, long n, long k, long *out) {
    if (k <= 0) {
        return;
    }
    if (k > n) {
        k = n;
    }
    if (k > n / 2) {
        long *pool = (long *)malloc((size_t)n * sizeof(long));
        for (long i = 0; i < n; i++) {
            pool[i] = i;
        }
        for (long i = 0; i < k; i++) {
            long j = i + rng_below(st, n - i);
            long tmp = pool[i];
            pool[i] = pool[j];
            pool[j] = tmp;
            out[i] = pool[i];
        }
        free(pool);
    } else {
        long_set selected;
        long_set_init(&selected, k);
        long count = 0;
        while (count < k) {
            long j = rng_below(st, n);
            if (long_set_add(&selected, j)) {
                out[count++] = j;
            }
        }
        long_set_free(&selected);
    }
}
