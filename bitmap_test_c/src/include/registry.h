/*
 * Strategy registry. Port of core/registry.py + allocators/__init__.py order.
 *
 * C has no decorators, so registration is a static table built in
 * registry_init.c, listed in the Python `preferred_order` (which determines
 * iteration order and each consistency group's reference member).
 */
#ifndef REGISTRY_H
#define REGISTRY_H

#include "base.h"

typedef allocator *(*allocator_factory)(long size);

typedef struct {
    const char *key;
    allocator_factory factory;
    const char *display_name;
} strategy_spec;

/* Returns the static spec array (preferred order); sets *count. */
const strategy_spec *registry_specs(long *count);

/* Lookup by key, or NULL if unknown. */
const strategy_spec *registry_get(const char *key);

#endif /* REGISTRY_H */
