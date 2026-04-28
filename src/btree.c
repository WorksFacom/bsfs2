#include "../include/btree.h"
#include "../bsfs.h"

/*
 * Static functions prototypes
 */
static int __btree_insert_nonfull(struct super_block *sb, btree_handle_t *handle, __u32 node_block, struct btree_node_t *node, void *entry);
static int __btree_split_child(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node_t *parent, int index);
static void __btree_remove_internal(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, int *success);
static void __btree_merge(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node_t *parent, int idx);
static void __btree_fill(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node_t *parent, int idx);
static void __btree_borrow_from_prev(struct super_block sb, btree_handle_t *handle, __u32 parent_block, struct btree_node_t *parent,
                                     int idx);
static void __btree_borrow_from_next(struct super_block sb, btree_handle_t *handle, __u32 parent_block, struct btree_node_t *parent,
                                     int idx);

static inline int default_compare(const void *a, const void *b) {
  __u32 ka = *(__u32 *)a;
  __u32 kb = *(__u32 *)b;
  return (ka < kb) ? -1 : (ka > kb);
}

static int compute_min_degree(__u64 entry_size) {
  __u64 entry_area   = entry_size * (2 * 4 - 1);
  __u64 pointer_area = sizeof(__u32) * (2 * 4);
  int   t            = 2;
  while (1) {
    entry_area          = entry_size * (2 * t - 1);
    pointer_area        = sizeof(__u32) * (2 * t);
    __u64 node_overhead = sizeof(__u8) + sizeof(__u32);
    if (entry_area + pointer_area + node_overhead > BLOCK_SIZE) {
      return t - 1;
    }
    t++;
  }
}

btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size, btree_compare_fn cmp, btree_key_fn get_key) {
  btree_handle_t *handle = kmalloc(sizeof(btree_handle_t), GFP_KERNEL);
  handle->type           = type;
  handle->entry_size     = entry_size;
  handle->cmp            = cmp ? cmp : default_compare;
  handle->get_key        = get_key;
  handle->root_block     = 0;
  handle->min_degree     = compute_min_degree(entry_size);
  return handle;
}

static inline void btree_shift_left(void *entries, __u64 entry_size, int from, int count) {
  for (int i = from; i < count - 1; i++) {
    memcpy((__u8 *)entries + i * entry_size, (__u8 *)entries + (i + 1) * entry_size, entry_size);
  }
}

__u32 hash_filename(const char *name) {
  __u32 hash = 5381;
  int   c;
  while ((c = *name++)) {
    hash = ((hash << 5) + hash) + c;
  }
  return hash;
}

// TODO: properly handle alignment (e.g: get_unaligned_le32)
struct btree_node *btree_read_node(struct super_block *sb, __u32 block) {
  struct buffer_head *bh;

  bh = sb_bread(sb, block);

  struct btree_node *node = kzalloc(sizeof(struct btree_node), GFP_KERNEL);

  node->bh   = bh;
  node->node = (struct btree_node_t *)bh->b_data;

  return node;
}

void btree_free_node(struct btree_node *node) {
  brelse(node->bh);
  kfree(node);
}

static inline __u8 *btree_entries(struct btree_node *node) {
  // up to: (2 * handle->min_degree - 1) * handle->entry_size;
  return node->node->data;
}

static inline __u32 *btree_children(struct btree_node *node, btree_handle_t *handle) {
  // up to: 2 * handle->min_degree

  size_t children_start = (2 * handle->min_degree - 1) * handle->entry_size;
  return (__u32 *)(node->node->data + children_start);
}

// TODO: is this working?
int btree_update(struct super_block *sb, btree_handle_t *handle, __u32 node_block, void *new_entry) {
  __u32  i, key, ret;
  size_t size;

  if (node_block == 0)
    return 0;

  key  = handle->get_key(new_entry);
  size = handle->entry_size;

  struct btree_node *node    = btree_read_node(sb, node_block);
  __u8              *entries = btree_entries(node);

  i = 0;
  while (i < node->node->num_entries) {
    __u32 entry_key = handle->get_key(entries + i * size);
    if (key <= entry_key)
      break;
    i++;
  }

  if (i < node->node->num_entries) {
    __u32 entry_key = handle->get_key(entries + i * size);
    // TODO: lock buffer!!!
    if (key == entry_key) {
      memcpy(entries + i * size, new_entry, size);

      mark_buffer_dirty(node->bh);
      ret = 1;
      goto out;
    }
  }

  if (node->node->is_leaf) {
    ret = 0;
    goto out;
  }

  __u32 *children = btree_children(node, handle);

  ret = btree_update(sb, handle, children[i], new_entry);

out:
  btree_free_node(node);
  return ret;
}

// TODO: duplicated code with btree_update
// TODO: is this working?
// TODO: remove recursion? not common in kernel
int btree_search(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, void *output) {
  __u32  i, ret;
  size_t size;

  if (node_block == 0)
    return 0;

  size = handle->entry_size;

  struct btree_node *node    = btree_read_node(sb, node_block);
  __u8              *entries = btree_entries(node);

  i = 0;
  while (i < node->node->num_entries) {
    __u32 entry_key = handle->get_key(entries + i * size);

    if (key <= entry_key)
      break;

    i++;
  }

  if (i < node->node->num_entries) {
    __u32 entry_key = handle->get_key(entries + i * size);

    if (key == entry_key) {
      memcpy(output, entries + i * size, size);

      ret = 1;
      goto out;
    }
  }

  if (node->node->is_leaf) {
    ret = 0;
    goto out;
  }

  __u32 *children = btree_children(node, handle);

  ret = btree_search(sb, handle, children[i], key, output);

out:
  btree_free_node(node);
  return ret;
}

int btree_search_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle, void *output) {
  int    i;
  __u32 *children;

  if (node_block == 0)
    return 0;

  struct btree_node *node = btree_read_node(sb, node_block);

  if (!node)
    return 0;

  __u8 *entries = btree_entries(node);

  i = 0;
  while (i < (int)node->node->num_entries) {
    void *e   = entries + i * handle->entry_size;
    int   cmp = handle->cmp(e, needle);
    (void)node_block; // suppress debug-only variables
    if (cmp >= 0)
      break;
    i++;
  }

  if (i < (int)node->node->num_entries) {
    void *e = entries + i * handle->entry_size;

    int cmp_eq = handle->cmp(e, needle);
    if (cmp_eq == 0) {
      memcpy(output, e, handle->entry_size);
      btree_free_node(node);
      return 1;
    }

    // If key > entries[i], descend to right child i+1
    if (!node->node->is_leaf && cmp_eq < 0) {
      children = btree_children(node, handle);
      // TODO: i think it's broken right here 👇
      __u32 right_child = children[i + 1];
      btree_free_node(node);
      return btree_search_entry(sb, handle, right_child, needle, output);
    }
  }

  if (node->node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  // Otherwise, key is less than entries[i] (or i == num_entries): go to child i
  children          = btree_children(node, handle);
  __u32 child_block = children[(__u32)i];
  btree_free_node(node);
  return btree_search_entry(sb, handle, child_block, needle, output);
}

int btree_insert(struct super_block *sb, btree_handle_t *handle, void *entry) {
  if (handle->root_block == 0) {
    struct btree_node_t *root = kmalloc(sizeof(struct btree_node_t), GFP_NOFS);

    if (!root)
      return -ENOMEM;

    root->is_leaf     = 1;
    root->num_entries = 0;

    // TODO: eu estava aqui

    handle->root_block = allocate_single_block(disk, sb);
    if (handle->root_block == INVALID_BLOCK) {
      btree_free_node(root);
      return -1;
    }

    if (__btree_insert_nonfull(disk, sb, handle, handle->root_block, root, entry) != 0 ||
        btree_write_node(disk, handle, handle->root_block, root) != 0) {
      btree_free_node(root);
      return -1;
    }

    btree_free_node(root);
    return 0;
  }

  btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
  if (!root)
    return -1;

  if (root->num_entries == (__u32)(2 * handle->min_degree - 1)) {
    btree_node_t *new_root = malloc(sizeof(btree_node_t));
    if (!new_root) {
      btree_free_node(root);
      return -1;
    }
    new_root->is_leaf     = 0;
    new_root->num_entries = 0;
    new_root->entries     = calloc(2 * handle->min_degree - 1, handle->entry_size);
    new_root->children    = calloc(2 * handle->min_degree, sizeof(__u32));
    if (!new_root->entries || !new_root->children) {
      btree_free_node(new_root);
      btree_free_node(root);
      return -1;
    }

    __u32 new_root_block = allocate_single_block(disk, sb);
    if (new_root_block == INVALID_BLOCK) {
      btree_free_node(new_root);
      btree_free_node(root);
      return -1;
    }

    new_root->children[0] = handle->root_block;

    if (__btree_split_child(disk, sb, handle, new_root_block, new_root, 0) != 0 ||
        __btree_insert_nonfull(disk, sb, handle, new_root_block, new_root, entry) != 0 ||
        btree_write_node(disk, handle, new_root_block, new_root) != 0) {
      btree_free_node(new_root);
      btree_free_node(root);
      return -1;
    }

    handle->root_block = new_root_block;
    btree_free_node(new_root);
  } else {
    if (__btree_insert_nonfull(disk, sb, handle, handle->root_block, root, entry) != 0 ||
        btree_write_node(disk, handle, handle->root_block, root) != 0) {
      btree_free_node(root);
      return -1;
    }
  }

  btree_free_node(root);
  return 0;
}

static int __btree_insert_nonfull(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 node_block, btree_node_t *node,
                                  void *entry) {
  int   i       = node->num_entries - 1;
  __u8 *entries = node->entries;
  __u32 key     = handle->get_key(entry);

  if (node->is_leaf) {
    while (i >= 0) {
      void *e   = entries + i * handle->entry_size;
      int   cmp = handle->cmp ? handle->cmp(e, entry) : ((int)handle->get_key(e) - (int)key);
      if (cmp <= 0)
        break;
      memcpy(entries + (i + 1) * handle->entry_size, entries + i * handle->entry_size, handle->entry_size);
      i--;
    }
    memcpy(entries + (i + 1) * handle->entry_size, entry, handle->entry_size);
    node->num_entries++;
    // persist leaf update immediately
    return btree_write_node(disk, handle, node_block, node);
  } else {
    while (i >= 0) {
      void *e   = entries + i * handle->entry_size;
      int   cmp = handle->cmp ? handle->cmp(e, entry) : ((int)handle->get_key(e) - (int)key);
      if (cmp <= 0)
        break;
      i--;
    }
    i++;
    __u32         child_block = node->children[i];
    btree_node_t *child       = btree_read_node(disk, handle, child_block);
    if (!child)
      return -1;

    if (child->num_entries == (__u32)(2 * handle->min_degree - 1)) {
      if (__btree_split_child(disk, sb, handle, node_block, node, i) != 0) {
        btree_free_node(child);
        return -1;
      }
      {
        void *mid = entries + i * handle->entry_size;
        int   cmp = handle->cmp ? handle->cmp(entry, mid) : ((int)key - (int)handle->get_key(mid));
        if (cmp > 0)
          i++;
      }
      btree_free_node(child);
      child_block = node->children[i];
      child       = btree_read_node(disk, handle, child_block);
      if (!child)
        return -1;
    }

    int result = __btree_insert_nonfull(disk, sb, handle, child_block, child, entry);
    if (result == 0) {
      // persist child and updated parent
      if (btree_write_node(disk, handle, child_block, child) != 0)
        result = -1;
      if (btree_write_node(disk, handle, node_block, node) != 0)
        result = -1;
    }

    btree_free_node(child);
    return result;
  }
}

static int __btree_split_child(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 parent_block, btree_node_t *parent,
                               int index) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[index]);
  if (!child)
    return -1;

  btree_node_t *new_node = malloc(sizeof(btree_node_t));
  if (!new_node) {
    btree_free_node(child);
    return -1;
  }
  new_node->is_leaf     = child->is_leaf;
  new_node->num_entries = handle->min_degree - 1;
  new_node->entries     = calloc(2 * handle->min_degree - 1, handle->entry_size);
  new_node->children    = calloc(2 * handle->min_degree, sizeof(__u32));
  if (!new_node->entries || !new_node->children) {
    btree_free_node(child);
    btree_free_node(new_node);
    return -1;
  }

  __u32 new_block = allocate_single_block(disk, sb);
  if (new_block == INVALID_BLOCK) {
    btree_free_node(child);
    btree_free_node(new_node);
    return -1;
  }

  memcpy(new_node->entries, (char *)child->entries + handle->min_degree * handle->entry_size,
         (handle->min_degree - 1) * handle->entry_size);

  if (!child->is_leaf) {
    memcpy(new_node->children, child->children + handle->min_degree, handle->min_degree * sizeof(__u32));
  }

  child->num_entries = handle->min_degree - 1;

  for (int j = (int)parent->num_entries; j > index; j--) {
    parent->children[j + 1] = parent->children[j];
  }
  parent->children[index + 1] = new_block;

  for (int j = (int)parent->num_entries - 1; j >= index; j--) {
    memcpy((char *)parent->entries + (j + 1) * handle->entry_size, (char *)parent->entries + j * handle->entry_size, handle->entry_size);
  }

  memcpy((char *)parent->entries + index * handle->entry_size, (char *)child->entries + (handle->min_degree - 1) * handle->entry_size,
         handle->entry_size);

  parent->num_entries++;

  int ok = 0;
  ok |= btree_write_node(disk, handle, parent->children[index], child);
  ok |= btree_write_node(disk, handle, new_block, new_node);
  ok |= btree_write_node(disk, handle, parent_block, parent);

  btree_free_node(child);
  btree_free_node(new_node);
  return (ok == 0) ? 0 : -1;
}

int btree_remove(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 key) {
  int   success  = 0;
  __u32 old_root = handle ? handle->root_block : 0;
  __btree_remove_internal(disk, sb, handle, handle->root_block, key, &success);

  if (success && handle && handle->root_block != 0) {
    btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
    if (root) {
      if (root->num_entries == 0) {
        if (root->is_leaf) {
          __u32 empty_block  = handle->root_block;
          handle->root_block = 0;
          (void)deallocate_single_block(disk, sb, empty_block);
        } else {
          __u32 new_root_block = root->children[0];
          __u32 empty_block    = handle->root_block;
          handle->root_block   = new_root_block;
          (void)deallocate_single_block(disk, sb, empty_block);
        }
      }
      btree_free_node(root);
    }
  } else if (success && handle && handle->root_block == 0 && old_root != 0) {
    (void)deallocate_single_block(disk, sb, old_root);
  }

  return success;
}

static void __btree_remove_internal(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, int *success) {
  if (node_block == 0) {
    *success = 0;
    return;
  }
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  __u32         i    = 0;
  while (i < node->num_entries) {
    __u32 ekey = handle->get_key((__u8 *)node->entries + i * handle->entry_size);
    if (key <= ekey)
      break;
    i++;
  }

  if (i < node->num_entries) {
    __u32 ekey = handle->get_key((__u8 *)node->entries + i * handle->entry_size);
    if (key == ekey) {
      if (node->is_leaf) {
        btree_shift_left(node->entries, handle->entry_size, i, node->num_entries);
        node->num_entries--;
        btree_write_node(disk, handle, node_block, node);
        *success = 1;
      } else {
        btree_node_t *pred = btree_read_node(disk, handle, node->children[i]);
        if (pred->num_entries >= (__u32)handle->min_degree) {
          memcpy((__u8 *)node->entries + i * handle->entry_size, (__u8 *)pred->entries + (pred->num_entries - 1) * handle->entry_size,
                 handle->entry_size);
          btree_write_node(disk, handle, node_block, node);
          __u32 pred_key = handle->get_key((__u8 *)pred->entries + (pred->num_entries - 1) * handle->entry_size);
          btree_free_node(pred);
          __btree_remove_internal(disk, sb, handle, node->children[i], pred_key, success);
        } else {
          btree_node_t *succ = btree_read_node(disk, handle, node->children[i + 1]);
          if (succ->num_entries >= (__u32)handle->min_degree) {
            memcpy((__u8 *)node->entries + i * handle->entry_size, (__u8 *)succ->entries, handle->entry_size);
            btree_write_node(disk, handle, node_block, node);
            __u32 succ_key = handle->get_key(succ->entries);
            btree_free_node(succ);
            __btree_remove_internal(disk, sb, handle, node->children[i + 1], succ_key, success);
          } else {
            __btree_merge(disk, sb, handle, node_block, node, i);
            __btree_remove_internal(disk, sb, handle, node->children[i], key, success);
          }
          // succ was freed in the >= branch and merged into left in the else
          // branch; do not double-free here.
        }
      }
    }
  } else {
    if (node->is_leaf) {
      *success = 0;
    } else {
      int           child_idx = i;
      btree_node_t *child     = btree_read_node(disk, handle, node->children[child_idx]);
      if (child->num_entries < (__u32)handle->min_degree) {
        __btree_fill(disk, sb, handle, node_block, node, child_idx);
        btree_write_node(disk, handle, node_block, node);
        if (child_idx > (int)node->num_entries)
          child_idx = node->num_entries;
      }
      btree_free_node(child);
      __btree_remove_internal(disk, sb, handle, node->children[child_idx], key, success);
    }
  }

  btree_free_node(node);
}

static int btree_find_max_entry(FILE *disk, btree_handle_t *handle, __u32 node_block, void *out_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;
  int ok;
  if (node->is_leaf) {
    if (node->num_entries > 0) {
      memcpy(out_entry, (__u8 *)node->entries + (node->num_entries - 1) * handle->entry_size, handle->entry_size);
      ok = 1;
    } else {
      ok = 0;
    }
  } else {
    ok = btree_find_max_entry(disk, handle, node->children[node->num_entries], out_entry);
  }
  btree_free_node(node);
  return ok;
}

static void btree_remove_entry_internal(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle,
                                        int *success) {
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node) {
    *success = 0;
    return;
  }

  __u32 i = 0;
  while (i < node->num_entries) {
    void *e   = (__u8 *)node->entries + i * handle->entry_size;
    int   cmp = handle->cmp(e, needle);
    if (cmp >= 0)
      break;
    i++;
  }

  int found_here = 0;
  if (i < node->num_entries) {
    void *e    = (__u8 *)node->entries + i * handle->entry_size;
    found_here = (handle->cmp(e, needle) == 0);
  }

  if (found_here) {
    // Case: found in this node
    if (node->is_leaf) {
      // Remove directly
      btree_shift_left(node->entries, handle->entry_size, (int)i, (int)node->num_entries);
      node->num_entries--;
      (void)btree_write_node(disk, handle, node_block, node);
      *success = 1;
      btree_free_node(node);
      return;
    }
    // Internal node: need predecessor or successor
    __u32         left_block  = node->children[i];
    __u32         right_block = node->children[i + 1];
    btree_node_t *left        = btree_read_node(disk, handle, left_block);
    btree_node_t *right       = btree_read_node(disk, handle, right_block);
    if (left && left->num_entries >= (__u32)handle->min_degree) {
      // Replace with predecessor
      __u8 pred[handle->entry_size];
      btree_free_node(right);
      int ok = btree_find_max_entry(disk, handle, left_block, pred);
      if (ok) {
        memcpy((__u8 *)node->entries + i * handle->entry_size, pred, handle->entry_size);
        (void)btree_write_node(disk, handle, node_block, node);
        btree_free_node(left);
        btree_remove_entry_internal(disk, sb, handle, left_block, pred, success);
        btree_free_node(node);
        return;
      }
    } else if (right && right->num_entries >= (__u32)handle->min_degree) {
      // Replace with successor
      __u8 succ[handle->entry_size];
      btree_free_node(left);
      // Find successor = minimum entry in right subtree
      // Local generic implementation
      int ok = 0;
      {
        __u32 nb = right_block;
        while (nb) {
          btree_node_t *n = btree_read_node(disk, handle, nb);
          if (!n)
            break;
          if (n->is_leaf) {
            if (n->num_entries > 0) {
              memcpy(succ, n->entries, handle->entry_size);
              ok = 1;
            }
            btree_free_node(n);
            break;
          }
          __u32 next = n->children[0];
          btree_free_node(n);
          nb = next;
        }
      }
      if (ok) {
        memcpy((__u8 *)node->entries + i * handle->entry_size, succ, handle->entry_size);
        (void)btree_write_node(disk, handle, node_block, node);
        btree_free_node(right);
        btree_remove_entry_internal(disk, sb, handle, right_block, succ, success);
        btree_free_node(node);
        return;
      }
    } else {
      // Merge left + key + right, then recurse into merged
      __btree_merge(disk, sb, handle, node_block, node, (int)i);
      // After merge, the merged child stays at children[i]
      __u32 merged_block = node->children[i];
      (void)btree_write_node(disk, handle, node_block, node);
      btree_remove_entry_internal(disk, sb, handle, merged_block, needle, success);
      btree_free_node(node);
      return;
    }
    // Defensive free
    if (left)
      btree_free_node(left);
    if (right)
      btree_free_node(right);
    btree_free_node(node);
    *success = 0;
    return;
  }

  // Not found in this node
  if (node->is_leaf) {
    btree_free_node(node);
    *success = 0;
    return;
  }

  // Choose child to descend
  int   child_idx       = (int)i;
  __u32 left_sib_block  = (child_idx > 0) ? node->children[child_idx - 1] : 0;
  __u32 child_block     = node->children[child_idx];
  __u32 right_sib_block = (child_idx < (int)node->num_entries) ? node->children[child_idx + 1] : 0;

  btree_node_t *child = btree_read_node(disk, handle, child_block);
  if (!child) {
    btree_free_node(node);
    *success = 0;
    return;
  }
  if (child->num_entries < (__u32)handle->min_degree) {
    // Try to borrow, else merge appropriately
    if (left_sib_block) {
      btree_node_t *left = btree_read_node(disk, handle, left_sib_block);
      if (left && left->num_entries >= (__u32)handle->min_degree) {
        __btree_borrow_from_prev(disk, handle, node_block, node, child_idx);
      } else {
        // Merge with left sibling
        __btree_merge(disk, sb, handle, node_block, node, child_idx - 1);
        child_idx -= 1;
        child_block = node->children[child_idx];
      }
      if (left)
        btree_free_node(left);
    } else if (right_sib_block) {
      btree_node_t *right = btree_read_node(disk, handle, right_sib_block);
      if (right && right->num_entries >= (__u32)handle->min_degree) {
        __btree_borrow_from_next(disk, handle, node_block, node, child_idx);
      } else {
        // Merge with right sibling at child_idx
        __btree_merge(disk, sb, handle, node_block, node, child_idx);
        // child_idx remains the same; merged block at children[child_idx]
        child_block = node->children[child_idx];
      }
      if (right)
        btree_free_node(right);
    }
    (void)btree_write_node(disk, handle, node_block, node);
  }
  if (child_idx > (int)node->num_entries)
    child_idx = node->num_entries;
  btree_free_node(child);

  // Recurse
  child_block = node->children[child_idx];
  if (child_block == 0) {
    btree_free_node(node);
    *success = 0;
    return;
  }
  btree_remove_entry_internal(disk, sb, handle, child_block, needle, success);
  btree_free_node(node);
}

int btree_remove_entry(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, const void *entry) {
  if (!handle || handle->root_block == 0)
    return 0;
  int success = 0;
  btree_remove_entry_internal(disk, sb, handle, handle->root_block, entry, &success);
  // Shrink root if empty
  if (handle->root_block != 0) {
    btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
    if (root) {
      if (root->num_entries == 0) {
        if (root->is_leaf) {
          __u32 old_root     = handle->root_block;
          handle->root_block = 0;
          (void)deallocate_single_block(disk, sb, old_root);
        } else {
          __u32 new_root_block = root->children[0];
          __u32 old_root       = handle->root_block;
          handle->root_block   = new_root_block;
          (void)deallocate_single_block(disk, sb, old_root);
        }
      }
      btree_free_node(root);
    }
  }
  return success;
}

static void __btree_merge(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 parent_block, btree_node_t *parent, int idx) {
  __u32 left_block  = parent->children[idx];
  __u32 right_block = parent->children[idx + 1];

  btree_node_t *left  = btree_read_node(disk, handle, left_block);
  btree_node_t *right = btree_read_node(disk, handle, right_block);

  __u32 left_entries = left->num_entries;

  memcpy((__u8 *)left->entries + left_entries * handle->entry_size, (__u8 *)parent->entries + idx * handle->entry_size, handle->entry_size);
  left_entries++;

  memcpy((__u8 *)left->entries + left_entries * handle->entry_size, (__u8 *)right->entries, handle->entry_size * right->num_entries);

  if (!left->is_leaf) {
    memcpy(left->children + left_entries, right->children, sizeof(__u32) * (right->num_entries + 1));
  }

  left->num_entries = left_entries + right->num_entries;

  for (__u32 i = (__u32)(idx + 1); i < parent->num_entries; i++) {
    memcpy((__u8 *)parent->entries + (i - 1) * handle->entry_size, (__u8 *)parent->entries + i * handle->entry_size, handle->entry_size);
    parent->children[i] = parent->children[i + 1];
  }

  parent->num_entries--;
  parent->children[parent->num_entries + 1] = 0;

  btree_write_node(disk, handle, left_block, left);
  btree_write_node(disk, handle, parent_block, parent);

  deallocate_single_block(disk, sb, right_block);

  btree_free_node(left);
  btree_free_node(right);
}

static void __btree_fill(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 parent_block, btree_node_t *parent, int idx) {
  if (idx > 0) {
    btree_node_t *left = btree_read_node(disk, handle, parent->children[idx - 1]);
    if (left->num_entries >= (__u32)handle->min_degree) {
      __btree_borrow_from_prev(disk, handle, parent_block, parent, idx);
      btree_free_node(left);
      return;
    }
    btree_free_node(left);
  }
  if ((__u32)idx < parent->num_entries) {
    btree_node_t *right = btree_read_node(disk, handle, parent->children[idx + 1]);
    if (right->num_entries >= (__u32)handle->min_degree) {
      __btree_borrow_from_next(disk, handle, parent_block, parent, idx);
      btree_free_node(right);
      return;
    }
    btree_free_node(right);
  }
  btree_merge(disk, sb, handle, parent_block, parent, (idx > 0) ? idx - 1 : idx);
}

static void __btree_borrow_from_prev(FILE *disk, btree_handle_t *handle, __u32 parent_block, btree_node_t *parent, int idx) {
  btree_node_t *child   = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *sibling = btree_read_node(disk, handle, parent->children[idx - 1]);

  for (int i = child->num_entries - 1; i >= 0; i--) {
    memcpy((__u8 *)child->entries + (i + 1) * handle->entry_size, (__u8 *)child->entries + i * handle->entry_size, handle->entry_size);
  }

  if (!child->is_leaf) {
    for (int i = child->num_entries; i >= 0; i--) {
      child->children[i + 1] = child->children[i];
    }
  }

  memcpy(child->entries, (__u8 *)parent->entries + (idx - 1) * handle->entry_size, handle->entry_size);

  if (!child->is_leaf) {
    child->children[0] = sibling->children[sibling->num_entries];
  }

  memcpy((__u8 *)parent->entries + (idx - 1) * handle->entry_size,
         (__u8 *)sibling->entries + (sibling->num_entries - 1) * handle->entry_size, handle->entry_size);

  child->num_entries++;
  sibling->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], child);
  btree_write_node(disk, handle, parent->children[idx - 1], sibling);
  btree_write_node(disk, handle, parent_block, parent);

  btree_free_node(child);
  btree_free_node(sibling);
}

static void __btree_borrow_from_next(FILE *disk, btree_handle_t *handle, __u32 parent_block, btree_node_t *parent, int idx) {
  btree_node_t *child   = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *sibling = btree_read_node(disk, handle, parent->children[idx + 1]);

  memcpy((__u8 *)child->entries + child->num_entries * handle->entry_size, (__u8 *)parent->entries + idx * handle->entry_size,
         handle->entry_size);

  if (!child->is_leaf) {
    child->children[child->num_entries + 1] = sibling->children[0];
  }

  memcpy((__u8 *)parent->entries + idx * handle->entry_size, sibling->entries, handle->entry_size);

  for (__u32 i = 1; i < sibling->num_entries; i++) {
    memcpy((__u8 *)sibling->entries + (i - 1) * handle->entry_size, (__u8 *)sibling->entries + i * handle->entry_size, handle->entry_size);
  }

  if (!sibling->is_leaf) {
    if (sibling->num_entries > 0) {
      __u32 end = sibling->num_entries - 1;
      for (__u32 i = 0; i <= end; i++) {
        sibling->children[i] = sibling->children[i + 1];
      }
    }
  }

  child->num_entries++;
  sibling->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], child);
  btree_write_node(disk, handle, parent->children[idx + 1], sibling);
  btree_write_node(disk, handle, parent_block, parent);

  btree_free_node(child);
  btree_free_node(sibling);
}

int btree_deallocate_all(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, __u32 node_block) {
  if (node_block == INVALID_BLOCK || node_block == 0)
    return 0;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node) {
    fprintf(stderr, "Failed to read B-tree node at block %u.\n", node_block);
    return -1;
  }

  // Recursively deallocate children
  if (!node->is_leaf) {
    for (__u32 i = 0; i <= node->num_entries; i++) {
      if (btree_deallocate_all(disk, sb, handle, node->children[i]) != 0) {
        btree_free_node(node);
        return -1;
      }
    }
  }

  // Deallocate data blocks if this is a BSPAN tree
  if (handle->type == BTREE_TYPE_BSPAN) {
    for (__u32 i = 0; i < node->num_entries; i++) {
      bspan_t *entry = (bspan_t *)(node->entries + i * handle->entry_size);
      if (entry->length == 0)
        continue;
      if (entry->disk_blk >= sb->total_blocks || entry->disk_blk + entry->length > sb->total_blocks) {
        fprintf(stderr,
                "btree_deallocate_all: skipping invalid span "
                "(disk=%u len=%u total=%u)\n",
                entry->disk_blk, entry->length, sb->total_blocks);
        continue;
      }
      if (deallocate_bspan(disk, sb, entry) != 0) {
        btree_free_node(node);
        return -1;
      }
    }
  }

  // Deallocate the node block itself
  if (deallocate_single_block(disk, sb, node_block) != 0) {
    btree_free_node(node);
    return -1;
  }

  btree_free_node(node);
  return 0;
}
