#include "../include/btree.h"
#include "../bsfs.h"

/*
 * Static functions prototypes
 */
static int  __btree_insert_nonfull(struct super_block *sb, btree_handle_t *handle, __u32 node_block, struct btree_node *node, void *entry);
static int  __btree_split_child(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent, int index);
static int  __btree_remove_internal(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key);
static void __btree_merge(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent, int idx);
static int  __btree_fill(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent, int idx);
static void __btree_borrow_from_prev(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent,
                                     int idx);
static void __btree_borrow_from_next(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent,
                                     int idx);

static inline int default_compare(const void *a, const void *b) {
  __u32 ka = *(__u32 *)a;
  __u32 kb = *(__u32 *)b;
  return (ka < kb) ? -1 : (ka > kb);
}

static int compute_min_degree(__u64 entry_size) {
  int t = 2;

  while (1) {
    __u64 entry_area    = entry_size * (2 * t - 1);
    __u64 pointer_area  = sizeof(__u32) * (2 * t);
    __u64 node_overhead = sizeof(__u8) + sizeof(__u32);

    if (entry_area + pointer_area + node_overhead > BSFS_BLOCK_SIZE)
      return t - 1;

    t++;
  }
}

btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size, btree_compare_fn cmp, btree_key_fn get_key) {
  btree_handle_t *handle = kmalloc(sizeof(btree_handle_t), GFP_KERNEL);

  if (!handle)
    return NULL;

  handle->type           = type;
  handle->entry_size     = entry_size;
  handle->cmp            = cmp ? cmp : default_compare;
  handle->get_key        = get_key;
  handle->root_block     = 0;
  handle->min_degree     = compute_min_degree(entry_size);
  return handle;
}

// buffer must be marked dirty afterwards
static inline void btree_shift_left(void *entries, __u64 entry_size, int from, int count) {
  memmove((__u8 *)entries + from * entry_size, (__u8 *)entries + (from + 1) * entry_size, (count - 1 - from) * entry_size);
}

// TODO: properly handle alignment (e.g: get_unaligned_le32)
int btree_read_node(struct super_block *sb, btree_handle_t *handle, __u32 block, struct btree_node **out_node) {
  struct buffer_head  *bh;
  struct btree_node_t *raw;

  *out_node = NULL;

  bh = sb_bread(sb, block);

  if (!bh)
    return -EIO;

  raw = (struct btree_node_t *)bh->b_data;

  if (raw->num_entries > (__u32)(2 * handle->min_degree - 1) || raw->is_leaf > 1) {
    pr_err("corrupt btree node at block %u (num_entries=%u is_leaf=%u)\n", block, raw->num_entries, raw->is_leaf);
    brelse(bh);
    return -EUCLEAN;
  }

  *out_node = kzalloc(sizeof(struct btree_node), GFP_KERNEL);

  if (!*out_node) {
    brelse(bh);
    return -ENOMEM;
  }

  (*out_node)->bh   = bh;
  (*out_node)->node = raw;

  return 0;
}

// grabs a freshly allocated block, zeroes it and marks it dirty; never reads
// from disk, so stale on-disk contents can't fail validation
static int btree_init_new_node(struct super_block *sb, __u32 block, struct btree_node **out_node) {
  struct buffer_head *bh;

  *out_node = NULL;

  bh = sb_getblk(sb, block);

  if (!bh)
    return -EIO;

  *out_node = kzalloc(sizeof(struct btree_node), GFP_KERNEL);

  if (!*out_node) {
    brelse(bh);
    return -ENOMEM;
  }

  lock_buffer(bh);
  memset(bh->b_data, 0, sb->s_blocksize);
  set_buffer_uptodate(bh);
  unlock_buffer(bh);
  mark_buffer_dirty(bh);

  (*out_node)->bh   = bh;
  (*out_node)->node = (struct btree_node_t *)bh->b_data;

  return 0;
}

void btree_free_node(struct btree_node *node) {
  brelse(node->bh);
  kfree(node);
}

// TODO: is this working?
int btree_update(struct super_block *sb, btree_handle_t *handle, __u32 node_block, void *new_entry) {
  int    ret;
  __u32  i, key;
  size_t size;

  struct btree_node *node;

  if (node_block == 0)
    return 0;

  key  = handle->get_key(new_entry);
  size = handle->entry_size;

  if (btree_read_node(sb, handle, node_block, &node) != 0)
    return 0;

  __u8 *entries = bentries(node);

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

  __u32 *children = bchildren(node, handle);

  ret = btree_update(sb, handle, children[i], new_entry);

out:
  btree_free_node(node);
  return ret;
}

// TODO: duplicated code with btree_update
// TODO: is this working?
int btree_search(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, void **output, struct btree_node **out_node) {
  __u32  i;
  __u32  next_block;
  size_t size;

  size = handle->entry_size;

  while (node_block != 0 && node_block != INVALID_BLOCK) {
    struct btree_node *node = NULL;

    if (btree_read_node(sb, handle, node_block, &node) != 0)
      return 0;

    __u8 *entries = bentries(node);

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
        *output   = entries + i * size;
        *out_node = node;
        // Keep the matching node alive so the returned pointer remains valid.
        // Ownership of this node is transferred to the caller.
        return 1;
      }
    }

    if (node->node->is_leaf) {
      btree_free_node(node);
      return 0;
    }

    next_block = bchildren(node, handle)[i];
    btree_free_node(node);
    node_block = next_block;
  }

  return 0;
}

int btree_search_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle, void *output) {
  int    i;
  __u32 *children;

  struct btree_node *node;

  if (node_block == 0)
    return 0;

  if (btree_read_node(sb, handle, node_block, &node) != 0)
    return 0;

  __u8 *entries = bentries(node);

  i = 0;
  while (i < (int)node->node->num_entries) {
    void *e   = entries + i * handle->entry_size;
    int   cmp = handle->cmp(e, needle);
    if (cmp >= 0)
      break;
    i++;
  }

  if (i < (int)node->node->num_entries && handle->cmp(entries + i * handle->entry_size, needle) == 0) {
    memcpy(output, entries + i * handle->entry_size, handle->entry_size);
    btree_free_node(node);
    return 1;
  }

  if (node->node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  // needle is less than entries[i] (or i == num_entries): go to child i
  children          = bchildren(node, handle);
  __u32 child_block = children[(__u32)i];
  btree_free_node(node);
  return btree_search_entry(sb, handle, child_block, needle, output);
}

// walks the btree to find the bspan with the greatest file_blk <= key
int bspan_search_le(struct super_block *sb, btree_handle_t *handle, uint32_t key, bspan_t *out) {
  int                have = 0;
  struct btree_node *node;
  bspan_t            best = {0};

  if (!handle || handle->root_block == 0)
    return 0;

  __u32 node_block = handle->root_block;
  while (node_block) {
    if (btree_read_node(sb, handle, node_block, &node) != 0)
      break;

    __u8 *entries = bentries(node);

    __u32 i = 0;
    while (i < node->node->num_entries) {
      __u32 ekey = handle->get_key(entries + i * handle->entry_size);

      if (ekey > key)
        break;

      memcpy(&best, entries + i * handle->entry_size, sizeof(best));
      have = 1;
      i++;
    }

    if (node->node->is_leaf) {
      btree_free_node(node);
      break;
    }

    __u32 child = bchildren(node, handle)[i];
    btree_free_node(node);
    node_block = child;
  }

  if (have) {
    *out = best;
    return 1;
  }

  return 0;
}

int btree_insert(struct super_block *sb, btree_handle_t *handle, void *entry) {
  int                ret;
  __u32              blocknr;
  struct btree_node *root;

  if (unlikely(handle->root_block == 0)) {
    blocknr = allocate_single_block(sb);

    if (blocknr == INVALID_BLOCK)
      return -ENOSPC;

    ret = btree_init_new_node(sb, blocknr, &root);

    if (ret != 0) {
      deallocate_single_block(sb, blocknr);
      return ret;
    }

    root->node->is_leaf = 1;
    handle->root_block  = blocknr;

    ret = __btree_insert_nonfull(sb, handle, blocknr, root, entry);
    btree_free_node(root);
    return ret;
  }

  ret = btree_read_node(sb, handle, handle->root_block, &root);

  if (ret != 0)
    return ret;

  if (root->node->num_entries == (__u32)(2 * handle->min_degree - 1)) {
    blocknr = allocate_single_block(sb);

    if (blocknr == INVALID_BLOCK) {
      btree_free_node(root);
      return -ENOSPC;
    }

    struct btree_node *new_root;
    ret = btree_init_new_node(sb, blocknr, &new_root);

    if (ret != 0) {
      deallocate_single_block(sb, blocknr);
      btree_free_node(root);
      return ret;
    }

    new_root->node->is_leaf     = 0;
    new_root->node->num_entries = 0;

    bchildren(new_root, handle)[0] = handle->root_block;

    if (__btree_split_child(sb, handle, blocknr, new_root, 0) != 0) {
      deallocate_single_block(sb, blocknr);
      btree_free_node(new_root);
      btree_free_node(root);
      return -EIO;
    }

    // the old root's upper half is only reachable through the new root now,
    // so the root switch must not depend on the insert below succeeding
    handle->root_block = blocknr;

    ret = __btree_insert_nonfull(sb, handle, blocknr, new_root, entry);
    btree_free_node(new_root);
  } else {
    ret = __btree_insert_nonfull(sb, handle, handle->root_block, root, entry);
  }

  btree_free_node(root);
  return ret;
}

static int __btree_insert_nonfull(struct super_block *sb, btree_handle_t *handle, __u32 node_block, struct btree_node *node, void *entry) {
  int ret;
  int i = node->node->num_entries - 1;

  __u8  *entries  = bentries(node);
  __u32 *children = bchildren(node, handle);

  struct btree_node *child;

  __u64 size = handle->entry_size;

  if (node->node->is_leaf) {
    while (i >= 0 && handle->cmp(entries + i * size, entry) > 0)
      i--;
    i++;

    memmove(entries + (i + 1) * size, entries + i * size, (node->node->num_entries - i) * size);
    memcpy(entries + i * size, entry, size);
    node->node->num_entries++;
    mark_buffer_dirty(node->bh);

    return 0;
  } else {
    while (i >= 0 && handle->cmp(entries + i * size, entry) > 0)
      i--;
    i++;

    __u32 child_block = children[i];
    ret               = btree_read_node(sb, handle, child_block, &child);

    if (ret != 0)
      return ret;

    if (child->node->num_entries == (__u32)(2 * handle->min_degree - 1)) {
      if (__btree_split_child(sb, handle, node_block, node, i) != 0) {
        btree_free_node(child);
        return -EIO;
      }

      if (handle->cmp(entry, entries + i * size) > 0)
        i++;

      btree_free_node(child);
      child_block = children[i];
      ret         = btree_read_node(sb, handle, child_block, &child);

      if (ret != 0)
        return ret;
    }

    ret = __btree_insert_nonfull(sb, handle, child_block, child, entry);

    btree_free_node(child);

    return ret;
  }
}

static int __btree_split_child(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent, int index) {
  int                ret;
  __u32              blocknr;
  struct btree_node *child, *new_node;

  __u64 size = handle->entry_size;

  __u8  *parent_entries  = bentries(parent);
  __u32 *parent_children = bchildren(parent, handle);

  blocknr = allocate_single_block(sb);

  if (blocknr == INVALID_BLOCK) {
    return -ENOSPC;
  }

  ret = btree_read_node(sb, handle, parent_children[index], &child);

  if (ret != 0) {
    deallocate_single_block(sb, blocknr);
    return ret;
  }

  ret = btree_init_new_node(sb, blocknr, &new_node);
  if (ret != 0) {
    btree_free_node(child);
    deallocate_single_block(sb, blocknr);
    return ret;
  }

  new_node->node->is_leaf     = child->node->is_leaf;
  new_node->node->num_entries = handle->min_degree - 1;

  memcpy(bentries(new_node), bentries(child) + handle->min_degree * size, (handle->min_degree - 1) * size);

  if (!child->node->is_leaf)
    memcpy(bchildren(new_node, handle), bchildren(child, handle) + handle->min_degree, handle->min_degree * sizeof(__u32));

  child->node->num_entries = handle->min_degree - 1;

  memmove(&parent_children[index + 2], &parent_children[index + 1], (parent->node->num_entries - index) * sizeof(__u32));
  parent_children[index + 1] = blocknr;

  memmove(parent_entries + (index + 1) * size, parent_entries + index * size, (parent->node->num_entries - index) * size);

  memcpy(parent_entries + index * size, bentries(child) + (handle->min_degree - 1) * size, size);

  parent->node->num_entries++;

  mark_buffer_dirty(child->bh);
  mark_buffer_dirty(new_node->bh);
  mark_buffer_dirty(parent->bh);

  btree_free_node(child);
  btree_free_node(new_node);
  return 0;
}

int btree_remove(struct super_block *sb, btree_handle_t *handle, __u32 key) {
  int                ret;
  struct btree_node *root;

  if (!handle || handle->root_block == 0)
    return 0;

  ret = __btree_remove_internal(sb, handle, handle->root_block, key);

  if (ret) {
    if (btree_read_node(sb, handle, handle->root_block, &root) == 0) {
      if (root->node->num_entries == 0) {
        __u32 empty_block = handle->root_block;

        if (root->node->is_leaf) {
          handle->root_block = 0;
        } else {
          handle->root_block = bchildren(root, handle)[0];
        }

        (void)deallocate_single_block(sb, empty_block);
      }

      btree_free_node(root);
    }
  }

  return ret;
}

static int __btree_remove_internal(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key) {
  int                ret = 0;
  __u32              i, ekey;
  struct btree_node *node, *pred, *succ, *child;

  if (node_block == 0)
    return 0;

  size_t size = handle->entry_size;

  if (btree_read_node(sb, handle, node_block, &node) != 0)
    return 0;

  i = 0;
  while (i < node->node->num_entries) {
    ekey = handle->get_key(bentries(node) + i * size);

    if (key <= ekey)
      break;

    i++;
  }

  if (i < node->node->num_entries) {
    ekey = handle->get_key(bentries(node) + i * size);

    if (key == ekey) {
      if (node->node->is_leaf) {
        btree_shift_left(bentries(node), size, i, node->node->num_entries);
        node->node->num_entries--;
        mark_buffer_dirty(node->bh);
        ret = 1;
      } else {
        if (btree_read_node(sb, handle, bchildren(node, handle)[i], &pred) != 0)
          goto out;

        if (pred->node->num_entries >= handle->min_degree) {
          memcpy(bentries(node) + i * size, bentries(pred) + (pred->node->num_entries - 1) * size, size);

          mark_buffer_dirty(node->bh);

          __u32 pred_key = handle->get_key(bentries(pred) + (pred->node->num_entries - 1) * size);

          btree_free_node(pred);

          ret = __btree_remove_internal(sb, handle, bchildren(node, handle)[i], pred_key);
        } else {
          btree_free_node(pred);

          if (btree_read_node(sb, handle, bchildren(node, handle)[i + 1], &succ) != 0)
            goto out;

          if (succ->node->num_entries >= handle->min_degree) {
            memcpy(bentries(node) + i * size, bentries(succ), size);

            mark_buffer_dirty(node->bh);

            __u32 succ_key = handle->get_key(bentries(succ));

            btree_free_node(succ);

            ret = __btree_remove_internal(sb, handle, bchildren(node, handle)[i + 1], succ_key);
          } else {
            __btree_merge(sb, handle, node_block, node, i);
            ret = __btree_remove_internal(sb, handle, bchildren(node, handle)[i], key);
          }
          // succ was freed in the >= branch and merged into left in the else
          // branch; do not double-free here.
        }
      }
    }
  } else {
    if (node->node->is_leaf) {
      ret = 0;
    } else {
      int child_idx = i;

      if (btree_read_node(sb, handle, bchildren(node, handle)[child_idx], &child) != 0)
        goto out;

      if (child->node->num_entries < (__u32)handle->min_degree)
        child_idx = __btree_fill(sb, handle, node_block, node, child_idx);

      btree_free_node(child);
      ret = __btree_remove_internal(sb, handle, bchildren(node, handle)[child_idx], key);
    }
  }

out:
  btree_free_node(node);

  return ret;
}

static int btree_find_max_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block, void *out_entry) {
  int                ret, ok;
  struct btree_node *node;
  __u8              *entries;
  __u32             *children;

  if (node_block == 0)
    return 0;

  ret = btree_read_node(sb, handle, node_block, &node);
  if (ret != 0)
    return 0;

  entries = bentries(node);
  if (node->node->is_leaf) {
    if (node->node->num_entries > 0) {
      memcpy(out_entry, entries + (node->node->num_entries - 1) * handle->entry_size, handle->entry_size);
      ok = 1;
    } else {
      ok = 0;
    }
  } else {
    children = bchildren(node, handle);
    ok       = btree_find_max_entry(sb, handle, children[node->node->num_entries], out_entry);
  }

  btree_free_node(node);
  return ok;
}

static void btree_remove_entry_internal(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle,
                                        int *success) {
  int                ret;
  __u32              i;
  __u64              size;
  __u8              *entries;
  __u32             *children;
  struct btree_node *node = NULL, *left = NULL, *right = NULL, *child = NULL;

  if (node_block == 0) {
    *success = 0;
    return;
  }

  ret = btree_read_node(sb, handle, node_block, &node);
  if (ret != 0) {
    *success = 0;
    return;
  }

  size    = handle->entry_size;
  entries = bentries(node);

  i = 0;
  while (i < node->node->num_entries) {
    void *e   = entries + i * size;
    int   cmp = handle->cmp(e, needle);
    if (cmp >= 0)
      break;
    i++;
  }

  if (i < node->node->num_entries && handle->cmp(entries + i * size, needle) == 0) {
    if (node->node->is_leaf) {
      btree_shift_left(entries, size, (int)i, (int)node->node->num_entries);
      node->node->num_entries--;
      mark_buffer_dirty(node->bh);
      *success = 1;
      goto out;
    } else {
      __u32 left_block, right_block;

      children    = bchildren(node, handle);
      left_block  = children[i];
      right_block = children[i + 1];

      ret = btree_read_node(sb, handle, left_block, &left);
      if (ret != 0) {
        *success = 0;
        goto out;
      }

      if (left->node->num_entries >= (__u32)handle->min_degree) {
        __u8 *pred = kmalloc(size, GFP_KERNEL);

        if (!pred) {
          *success = 0;
          goto out;
        }

        if (btree_find_max_entry(sb, handle, left_block, pred)) {
          memcpy(entries + i * size, pred, size);
          mark_buffer_dirty(node->bh);

          btree_free_node(left);
          left = NULL;
          btree_free_node(node);
          node = NULL;

          btree_remove_entry_internal(sb, handle, left_block, pred, success);
          kfree(pred);
          return;
        }

        kfree(pred);
        *success = 0;
        goto out;
      }

      btree_free_node(left);
      left = NULL;

      ret = btree_read_node(sb, handle, right_block, &right);
      if (ret != 0) {
        *success = 0;
        goto out;
      }

      if (right->node->num_entries >= (__u32)handle->min_degree) {
        __u8 *succ = kmalloc(size, GFP_KERNEL);
        int   ok   = 0;

        if (!succ) {
          *success = 0;
          goto out;
        }

        {
          __u32 nb = right_block;
          while (nb) {
            struct btree_node *n;

            ret = btree_read_node(sb, handle, nb, &n);
            if (ret != 0)
              break;

            if (n->node->is_leaf) {
              if (n->node->num_entries > 0) {
                memcpy(succ, bentries(n), size);
                ok = 1;
              }
              btree_free_node(n);
              break;
            }

            nb = bchildren(n, handle)[0];
            btree_free_node(n);
          }
        }

        if (ok) {
          memcpy(entries + i * size, succ, size);
          mark_buffer_dirty(node->bh);

          btree_free_node(right);
          right = NULL;
          btree_free_node(node);
          node = NULL;

          btree_remove_entry_internal(sb, handle, right_block, succ, success);
          kfree(succ);
          return;
        }

        kfree(succ);
        *success = 0;
        goto out;
      }

      btree_free_node(right);
      right = NULL;

      __btree_merge(sb, handle, node_block, node, (int)i);
      children = bchildren(node, handle);
      {
        __u32 merged_block = children[i];
        btree_free_node(node);
        btree_remove_entry_internal(sb, handle, merged_block, needle, success);
        return;
      }
    }
  }

  if (node->node->is_leaf) {
    *success = 0;
    goto out;
  }

  {
    int   child_idx;
    __u32 child_block;

    child_idx = (int)i;
    children  = bchildren(node, handle);
    ret       = btree_read_node(sb, handle, children[child_idx], &child);
    if (ret != 0) {
      *success = 0;
      goto out;
    }

    if (child->node->num_entries < (__u32)handle->min_degree)
      child_idx = __btree_fill(sb, handle, node_block, node, child_idx);

    btree_free_node(child);
    child = NULL;

    children    = bchildren(node, handle);
    child_block = children[child_idx];

    btree_free_node(node);
    node = NULL;

    if (child_block == 0) {
      *success = 0;
      return;
    }

    btree_remove_entry_internal(sb, handle, child_block, needle, success);
    return;
  }

out:
  if (child)
    btree_free_node(child);
  if (right)
    btree_free_node(right);
  if (left)
    btree_free_node(left);
  if (node)
    btree_free_node(node);
}

int btree_remove_entry(struct super_block *sb, btree_handle_t *handle, const void *entry) {
  int                success;
  struct btree_node *root;

  if (!handle || handle->root_block == 0)
    return 0;

  success = 0;
  btree_remove_entry_internal(sb, handle, handle->root_block, entry, &success);

  if (success && handle->root_block != 0) {
    if (btree_read_node(sb, handle, handle->root_block, &root) == 0) {
      if (root->node->num_entries == 0) {
        __u32 old_root = handle->root_block;

        if (root->node->is_leaf) {
          handle->root_block = 0;
        } else {
          handle->root_block = bchildren(root, handle)[0];
        }

        (void)deallocate_single_block(sb, old_root);
      }

      btree_free_node(root);
    }
  }

  return success;
}

static void __btree_merge(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent, int idx) {
  __u32 left_block  = bchildren(parent, handle)[idx];
  __u32 right_block = bchildren(parent, handle)[idx + 1];

  size_t size = handle->entry_size;

  struct btree_node *left, *right;

  if (btree_read_node(sb, handle, left_block, &left) != 0)
    return;

  if (btree_read_node(sb, handle, right_block, &right) != 0) {
    btree_free_node(left);
    return;
  }

  __u32 left_entries = left->node->num_entries;

  memcpy(bentries(left) + left_entries * size, bentries(parent) + idx * size, size);
  left_entries++;

  memcpy(bentries(left) + left_entries * size, bentries(right), size * right->node->num_entries);

  if (!left->node->is_leaf)
    memcpy(bchildren(left, handle) + left_entries, bchildren(right, handle), sizeof(__u32) * (right->node->num_entries + 1));

  left->node->num_entries = left_entries + right->node->num_entries;

  {
    __u32 *c     = bchildren(parent, handle);
    __u32  moved = parent->node->num_entries - (idx + 1);

    memmove(bentries(parent) + idx * size, bentries(parent) + (idx + 1) * size, moved * size);
    memmove(&c[idx + 1], &c[idx + 2], moved * sizeof(__u32));
  }

  parent->node->num_entries--;
  bchildren(parent, handle)[parent->node->num_entries + 1] = 0;

  mark_buffer_dirty(left->bh);
  mark_buffer_dirty(parent->bh);

  deallocate_single_block(sb, right_block);

  btree_free_node(left);
  btree_free_node(right);
}

// rebalances the underfull child at children[idx] by borrowing from a sibling
// or merging with one; returns the index where that child lives afterwards
// (idx after a borrow, idx - 1 after a merge into the left sibling)
static int __btree_fill(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent, int idx) {
  int                ret;
  __u32             *children;
  struct btree_node *left = NULL, *right = NULL;

  children = bchildren(parent, handle);

  if (idx > 0) {
    ret = btree_read_node(sb, handle, children[idx - 1], &left);
    if (ret == 0 && left->node->num_entries >= (__u32)handle->min_degree) {
      __btree_borrow_from_prev(sb, handle, parent_block, parent, idx);
      btree_free_node(left);
      return idx;
    }
    if (left)
      btree_free_node(left);
  }

  if ((__u32)idx < parent->node->num_entries) {
    ret = btree_read_node(sb, handle, children[idx + 1], &right);
    if (ret == 0 && right->node->num_entries >= (__u32)handle->min_degree) {
      __btree_borrow_from_next(sb, handle, parent_block, parent, idx);
      btree_free_node(right);
      return idx;
    }
    if (right)
      btree_free_node(right);
  }

  if (idx > 0) {
    __btree_merge(sb, handle, parent_block, parent, idx - 1);
    return idx - 1;
  }

  __btree_merge(sb, handle, parent_block, parent, idx);
  return idx;
}

static void __btree_borrow_from_prev(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent,
                                     int idx) {
  int                ret;
  __u64              size;
  __u8              *parent_entries, *child_entries, *sibling_entries;
  __u32             *parent_children, *child_children, *sibling_children;
  struct btree_node *child = NULL, *sibling = NULL;

  (void)parent_block;

  size            = handle->entry_size;
  parent_entries  = bentries(parent);
  parent_children = bchildren(parent, handle);

  ret = btree_read_node(sb, handle, parent_children[idx], &child);
  if (ret != 0)
    return;

  ret = btree_read_node(sb, handle, parent_children[idx - 1], &sibling);
  if (ret != 0) {
    btree_free_node(child);
    return;
  }

  child_entries   = bentries(child);
  sibling_entries = bentries(sibling);

  memmove(child_entries + size, child_entries, child->node->num_entries * size);

  child_children = bchildren(child, handle);
  if (!child->node->is_leaf)
    memmove(&child_children[1], &child_children[0], (child->node->num_entries + 1) * sizeof(__u32));

  memcpy(child_entries, parent_entries + (idx - 1) * size, size);

  sibling_children = bchildren(sibling, handle);
  if (!child->node->is_leaf) {
    child_children[0] = sibling_children[sibling->node->num_entries];
  }

  memcpy(parent_entries + (idx - 1) * size, sibling_entries + (sibling->node->num_entries - 1) * size, size);

  child->node->num_entries++;
  sibling->node->num_entries--;

  mark_buffer_dirty(child->bh);
  mark_buffer_dirty(sibling->bh);
  mark_buffer_dirty(parent->bh);

  btree_free_node(child);
  btree_free_node(sibling);
}

static void __btree_borrow_from_next(struct super_block *sb, btree_handle_t *handle, __u32 parent_block, struct btree_node *parent,
                                     int idx) {
  int                ret;
  __u64              size;
  __u8              *parent_entries, *child_entries, *sibling_entries;
  __u32             *parent_children, *child_children, *sibling_children;
  struct btree_node *child = NULL, *sibling = NULL;

  (void)parent_block;

  size            = handle->entry_size;
  parent_entries  = bentries(parent);
  parent_children = bchildren(parent, handle);

  ret = btree_read_node(sb, handle, parent_children[idx], &child);
  if (ret != 0)
    return;

  ret = btree_read_node(sb, handle, parent_children[idx + 1], &sibling);
  if (ret != 0) {
    btree_free_node(child);
    return;
  }

  child_entries   = bentries(child);
  sibling_entries = bentries(sibling);

  memcpy(child_entries + child->node->num_entries * size, parent_entries + idx * size, size);

  child_children   = bchildren(child, handle);
  sibling_children = bchildren(sibling, handle);
  if (!child->node->is_leaf) {
    child_children[child->node->num_entries + 1] = sibling_children[0];
  }

  memcpy(parent_entries + idx * size, sibling_entries, size);

  memmove(sibling_entries, sibling_entries + size, (sibling->node->num_entries - 1) * size);

  if (!sibling->node->is_leaf)
    memmove(&sibling_children[0], &sibling_children[1], sibling->node->num_entries * sizeof(__u32));

  child->node->num_entries++;
  sibling->node->num_entries--;

  mark_buffer_dirty(child->bh);
  mark_buffer_dirty(sibling->bh);
  mark_buffer_dirty(parent->bh);

  btree_free_node(child);
  btree_free_node(sibling);
}

int btree_deallocate_all(struct super_block *sb, btree_handle_t *handle, __u32 node_block) {
  int                     ret;
  __u8                   *entries;
  __u32                  *children;
  struct btree_node      *node;
  struct bsfs_super_info *sbi;

  if (node_block == INVALID_BLOCK || node_block == 0)
    return 0;

  ret = btree_read_node(sb, handle, node_block, &node);
  if (ret != 0) {
    printk(KERN_ERR "BSFS: failed to read B-tree node at block %u\n", node_block);
    return -1;
  }

  if (!node->node->is_leaf) {
    children = bchildren(node, handle);
    for (__u32 i = 0; i <= node->node->num_entries; i++) {
      if (btree_deallocate_all(sb, handle, children[i]) != 0) {
        btree_free_node(node);
        return -1;
      }
    }
  }

  if (handle->type == BTREE_TYPE_BSPAN) {
    sbi = bsfs_sb(sb);
    if (!sbi || !sbi->super) {
      btree_free_node(node);
      return -1;
    }

    entries = bentries(node);
    for (__u32 i = 0; i < node->node->num_entries; i++) {
      bspan_t *entry = (bspan_t *)(entries + i * handle->entry_size);
      if (entry->length == 0)
        continue;
      if (entry->disk_blk >= sbi->super->total_blocks || entry->disk_blk + entry->length > sbi->super->total_blocks) {
        printk(KERN_WARNING "BSFS: btree_deallocate_all skipping invalid span (disk=%u len=%u total=%u)\n", entry->disk_blk, entry->length,
               sbi->super->total_blocks);
        continue;
      }
      if (deallocate_bspan(sb, entry) != 0) {
        btree_free_node(node);
        return -1;
      }
    }
  }

  if (deallocate_single_block(sb, node_block) != 0) {
    btree_free_node(node);
    return -1;
  }

  btree_free_node(node);
  return 0;
}
