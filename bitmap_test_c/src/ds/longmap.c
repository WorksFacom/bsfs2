#include "longmap.h"

#include <stdlib.h>

#define LM_INIT_CAP 16

void lm_init(longmap *m) {
    m->cap = LM_INIT_CAP;
    m->count = 0;
    m->slots = (lm_entry *)calloc((size_t)m->cap, sizeof(lm_entry));
}

void lm_free(longmap *m) {
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

static long lm_hash(long key, long cap) {
    unsigned long h = (unsigned long)key * 0x9E3779B97F4A7C15UL;
    return (long)(h & (unsigned long)(cap - 1));
}

static long lm_find_slot(const longmap *m, long key) {
    long mask = m->cap - 1;
    long pos = lm_hash(key, m->cap);
    while (m->slots[pos].used) {
        if (m->slots[pos].key == key) {
            return pos;
        }
        pos = (pos + 1) & mask;
    }
    return pos; /* empty slot where key would go */
}

long lm_get(const longmap *m, long key, long def) {
    long pos = lm_find_slot(m, key);
    if (m->slots[pos].used) {
        return m->slots[pos].val;
    }
    return def;
}

int lm_contains(const longmap *m, long key) {
    long pos = lm_find_slot(m, key);
    return m->slots[pos].used;
}

static void lm_resize(longmap *m, long new_cap) {
    lm_entry *old = m->slots;
    long old_cap = m->cap;
    m->slots = (lm_entry *)calloc((size_t)new_cap, sizeof(lm_entry));
    m->cap = new_cap;
    m->count = 0;
    for (long i = 0; i < old_cap; i++) {
        if (old[i].used) {
            long pos = lm_find_slot(m, old[i].key);
            m->slots[pos].used = 1;
            m->slots[pos].key = old[i].key;
            m->slots[pos].val = old[i].val;
            m->count++;
        }
    }
    free(old);
}

void lm_set(longmap *m, long key, long val) {
    long pos = lm_find_slot(m, key);
    if (m->slots[pos].used) {
        m->slots[pos].val = val;
        return;
    }
    if ((m->count + 1) * 10 >= m->cap * 7) {
        lm_resize(m, m->cap * 2);
        pos = lm_find_slot(m, key);
    }
    m->slots[pos].used = 1;
    m->slots[pos].key = key;
    m->slots[pos].val = val;
    m->count++;
}

void lm_del(longmap *m, long key) {
    long mask = m->cap - 1;
    long pos = lm_find_slot(m, key);
    if (!m->slots[pos].used) {
        return;
    }
    /* backward-shift deletion */
    m->slots[pos].used = 0;
    m->count--;
    long next = (pos + 1) & mask;
    while (m->slots[next].used) {
        long home = lm_hash(m->slots[next].key, m->cap);
        /* if `next` can be moved into the hole at `pos` */
        int can_move;
        if (pos <= next) {
            can_move = !(pos < home && home <= next);
        } else {
            can_move = !(pos < home || home <= next);
        }
        if (can_move) {
            m->slots[pos] = m->slots[next];
            m->slots[next].used = 0;
            pos = next;
        }
        next = (next + 1) & mask;
    }
}
