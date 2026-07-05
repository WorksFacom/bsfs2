#include "stats.h"

#include <stdlib.h>

#include "rng.h"

long *generate_seeds(long num_seeds, long seed_base) {
    long *seeds = (long *)malloc((size_t)num_seeds * sizeof(long));
    rng_state st;
    rng_seed(&st, (uint64_t)seed_base);
    const long upper = (1L << 32) - 1; /* randrange(0, 2**32 - 1) */
    for (long i = 0; i < num_seeds; i++) {
        seeds[i] = rng_below(&st, upper);
    }
    return seeds;
}
