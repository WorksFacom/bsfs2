/*
 * strategy registry, port of core/registry.py + the allocators/__init__.py order.
 *
 * no decorators in C, so it's a static table in core/registry.c in Python's `preferred_order`. that
 * order decides iteration and which member is each consistency group's reference.
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

/* static spec array in preferred order, sets *count */
const strategy_spec *registry_specs(long *count);

/* NULL if unknown */
const strategy_spec *registry_get(const char *key);

#endif /* REGISTRY_H */
