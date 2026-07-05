/*
 * Open-addressing hash map: long key -> long value.
 * Used by the segment-tree allocator (node index -> recycled count) and the
 * hybrid allocator (block index -> heap position).
 */
#ifndef LONGMAP_H
#define LONGMAP_H

typedef struct {
    long key;
    long val;
    int used;
} lm_entry;

typedef struct {
    lm_entry *slots;
    long cap;
    long count;
} longmap;

void lm_init(longmap *m);
void lm_free(longmap *m);
long lm_get(const longmap *m, long key, long def);
int lm_contains(const longmap *m, long key);
void lm_set(longmap *m, long key, long val);
void lm_del(longmap *m, long key);

#endif /* LONGMAP_H */
