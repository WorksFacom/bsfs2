#include "bench_util.h"

#include <string.h>

uint64_t bench_splitmix64(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

void bench_pattern_fill(void *buffer, size_t length, uint64_t seed,
                        uint64_t stream, uint64_t offset) {
  unsigned char *bytes = buffer;
  uint64_t word_index = offset / sizeof(uint64_t);
  size_t prefix = (size_t)(offset % sizeof(uint64_t));
  size_t produced = 0;

  while (produced < length) {
    uint64_t word =
        bench_splitmix64(seed ^ (stream * 0xd6e8feb86659fd93ULL) ^ word_index);
    unsigned char encoded[sizeof(word)];
    size_t begin = (produced == 0) ? prefix : 0;

    memcpy(encoded, &word, sizeof(word));
    for (size_t i = begin; i < sizeof(word) && produced < length; i++)
      bytes[produced++] = encoded[i];
    word_index++;
  }
}

void bench_shuffle_u32(uint32_t *values, size_t count, uint64_t seed) {
  uint64_t state = seed;

  if (!values || count < 2)
    return;
  for (size_t i = count - 1; i > 0; i--) {
    size_t j;
    uint32_t tmp;

    state = bench_splitmix64(state);
    j = (size_t)(state % (i + 1));
    tmp = values[i];
    values[i] = values[j];
    values[j] = tmp;
  }
}
