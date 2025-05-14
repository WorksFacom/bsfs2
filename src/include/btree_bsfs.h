#ifndef BTREE_H
#define BTREE_H

#include "allocate_bsfs.h"
#include "block_operations_bsfs.h"
#include "filesystem_bsfs.h"
#define MAX_BSPANS_PER_NODE 255
#define MAX_CHILDREN_PER_NODE (MAX_BSPANS_PER_NODE + 1)

typedef enum {
  BSPAN,
  INODE,
  REC_ENTRY,
} btree_data_type_t;

typedef struct btree_node {
  uint8_t is_leaf;
  uint32_t parent;
  uint32_t num_entries;
  void *entries;
  uint32_t *children;
} btree_node_t;

#endif // !BTREE_H
