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
  struct buffer_head      *bh;
  struct btree_node_t     *raw;
  struct bsfs_super_info  *sbi = bsfs_sb(sb);

  *out_node = NULL;

  // corrupt child pointer must not read an arbitrary block as a node
  if (block == 0 || block == INVALID_BLOCK || (sbi && sbi->super && block >= sbi->super->total_blocks)) {
    pr_err("refusing to read btree node at invalid block %u\n", block);
    return -EUCLEAN;
  }

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

int btree_update(struct super_block *sb, btree_handle_t *handle, __u32 node_block, void *new_entry) {
  int    ret;
  __u32  i, key;
  size_t size;

  struct btree_node *node;

  if (node_block == 0)
    return 0;

  key  = handle->get_key(new_entry);
  size = handle->entry_size;

  ret = btree_read_node(sb, handle, node_block, &node);

  if (ret != 0)
    return ret;

  __u8 *entries = bentries(node);

  i = 0;
  while (i < node->node->num_entries) {
    __u32 entry_key = handle->get_key(entries + i * size);
    if (key <= entry_key)
      break;
    i++;
  }

  if (node->node->is_leaf) {
    if (i < node->node->num_entries && handle->get_key(entries + i * size) == key) {
      memcpy(entries + i * size, new_entry, size);
      mark_buffer_dirty(node->bh);
      ret = 1;
    } else {
      ret = 0;
    }
    goto out;
  }

  __u32 *children = bchildren(node, handle);
  __u32  descend  = (i < node->node->num_entries && handle->get_key(entries + i * size) == key) ? children[i + 1] : children[i];

  ret = btree_update(sb, handle, descend, new_entry);

out:
  btree_free_node(node);
  return ret;
}

// TODO: duplicated code with btree_update
// TODO: is this working?
int btree_search(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key, void **output, struct btree_node **out_node) {
  int    ret;
  __u32  i;
  __u32  next_block;
  size_t size;

  size = handle->entry_size;

  while (node_block != 0 && node_block != INVALID_BLOCK) {
    struct btree_node *node = NULL;

    ret = btree_read_node(sb, handle, node_block, &node);

    if (ret != 0)
      return ret;

    __u8 *entries = bentries(node);

    i = 0;
    while (i < node->node->num_entries) {
      __u32 entry_key = handle->get_key(entries + i * size);

      if (key <= entry_key)
        break;

      i++;
    }

    if (node->node->is_leaf) {
      if (i < node->node->num_entries && handle->get_key(entries + i * size) == key) {
        *output   = entries + i * size;
        *out_node = node;
        // Ownership of this leaf node is transferred to the caller.
        return 1;
      }
      btree_free_node(node);
      return 0;
    }

    // Internal node: separators are copies of leaf entries; keys equal to a
    // separator live in the right subtree (see split_child leaf convention).
    if (i < node->node->num_entries && handle->get_key(entries + i * size) == key)
      next_block = bchildren(node, handle)[i + 1];
    else
      next_block = bchildren(node, handle)[i];
    btree_free_node(node);
    node_block = next_block;
  }

  return 0;
}

int btree_search_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle, void *output) {
  int    i, ret;
  __u32 *children;

  struct btree_node *node;

  if (node_block == 0)
    return 0;

  ret = btree_read_node(sb, handle, node_block, &node);

  if (ret != 0)
    return ret;

  __u8 *entries = bentries(node);

  i = 0;
  while (i < (int)node->node->num_entries) {
    void *e   = entries + i * handle->entry_size;
    int   cmp = handle->cmp(e, needle);
    if (cmp >= 0)
      break;
    i++;
  }

  if (node->node->is_leaf) {
    if (i < (int)node->node->num_entries) {
      void *e = entries + i * handle->entry_size;
      if (handle->cmp(e, needle) == 0) {
        memcpy(output, e, handle->entry_size);
        btree_free_node(node);
        return 1;
      }
    }
    btree_free_node(node);
    return 0;
  }

  // Internal: descend right on separator equality (separator lives in right leaf).
  children = bchildren(node, handle);
  __u32 child_block;
  if (i < (int)node->node->num_entries && handle->cmp(entries + i * handle->entry_size, needle) == 0)
    child_block = children[i + 1];
  else
    child_block = children[(__u32)i];
  btree_free_node(node);
  return btree_search_entry(sb, handle, child_block, needle, output);
}

// walks the btree to find the bspan with the greatest file_blk <= key
// a mid-walk failure must not return the partial best-so-far or callers would map/allocate against a stale span
int bspan_search_le(struct super_block *sb, btree_handle_t *handle, uint32_t key, bspan_t *out) {
  int                ret, have = 0;
  struct btree_node *node;
  bspan_t            best = {0};

  if (!handle || handle->root_block == 0)
    return 0;

  __u32 node_block = handle->root_block;
  while (node_block) {
    ret = btree_read_node(sb, handle, node_block, &node);

    if (ret != 0)
      return ret;

    __u8 *entries = bentries(node);

    __u32 i = 0;
    while (i < node->node->num_entries) {
      __u32 ekey = handle->get_key(entries + i * handle->entry_size);
      if (ekey > key)
        break;
      i++;
    }

    if (node->node->is_leaf) {
      // Only leaf entries hold authoritative data in a B+ tree; take the
      // largest entry <= key from this leaf.
      if (i > 0) {
        memcpy(&best, entries + (i - 1) * handle->entry_size, sizeof(best));
        have = 1;
      }
      btree_free_node(node);
      break;
    }

    // Internal node: separator == key means the entry lives in the right subtree.
    __u32 child;
    if (i < node->node->num_entries && handle->get_key(entries + i * handle->entry_size) == key)
      child = bchildren(node, handle)[i + 1];
    else
      child = bchildren(node, handle)[i];
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

      {
        // B+: the separator just inserted is a copy of the right leaf's first
        // entry, so an entry equal to it belongs in the right child.
        void *mid = entries + i * size;

        if (handle->cmp(entry, mid) >= 0)
          i++;
      }
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

  ret = btree_read_node(sb, handle, blocknr, &new_node);
  if (ret != 0) {
    btree_free_node(child);
    deallocate_single_block(sb, blocknr);
    return ret;
  }

  new_node->node->is_leaf     = child->node->is_leaf;
  new_node->node->num_entries = handle->min_degree - 1;

  // Right half: entries [t, 2t-2] move to the new node in both cases.
  memcpy(bentries(new_node), (char *)bentries(child) + handle->min_degree * size, (handle->min_degree - 1) * size);

  if (!child->node->is_leaf)
    memcpy(bchildren(new_node, handle), bchildren(child, handle) + handle->min_degree, handle->min_degree * sizeof(__u32));

  // B+: in a leaf no record is promoted, so the child keeps t entries (the one
  // at index t-1 is still data). In an internal node the median moves up to the
  // parent, so the child keeps t-1.
  child->node->num_entries = child->node->is_leaf ? handle->min_degree : handle->min_degree - 1;

  memmove(&parent_children[index + 2], &parent_children[index + 1], (parent->node->num_entries - index) * sizeof(__u32));
  parent_children[index + 1] = blocknr;

  memmove(parent_entries + (index + 1) * size, parent_entries + index * size, (parent->node->num_entries - index) * size);

  // Separator: in a leaf it is a COPY of the right leaf's first entry (the
  // record stays there); in an internal node it is the median, moved up.
  if (child->node->is_leaf)
    memcpy((char *)parent_entries + index * size, bentries(new_node), size);
  else
    memcpy((char *)parent_entries + index * size, (char *)bentries(child) + (handle->min_degree - 1) * size, size);

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

/*
 * Index of the child to descend into to reach `needle`.
 *
 * B+ convention of this tree (the same one used by btree_search_entry and by
 * the split): an internal node's separator is a COPY of the first entry of the
 * leaf to its right, so a key equal to the separator lives in the RIGHT
 * subtree.
 */
static __u32 btree_descend_index_cmp(struct btree_node *node, btree_handle_t *handle, const void *needle) {
  __u8 *entries = bentries(node);
  __u32 i       = 0;

  while (i < node->node->num_entries && handle->cmp(entries + i * handle->entry_size, needle) < 0)
    i++;

  if (i < node->node->num_entries && handle->cmp(entries + i * handle->entry_size, needle) == 0)
    i++;

  return i;
}

static __u32 btree_descend_index_key(struct btree_node *node, btree_handle_t *handle, __u32 key) {
  __u8 *entries = bentries(node);
  __u32 i       = 0;

  while (i < node->node->num_entries && handle->get_key(entries + i * handle->entry_size) < key)
    i++;

  if (i < node->node->num_entries && handle->get_key(entries + i * handle->entry_size) == key)
    i++;

  return i;
}

/*
 * B+ removal by key - same shape as the comparator version above: descend to
 * the leaf, rebalancing children along the way; only the leaf deletes.
 */
static int __btree_remove_internal(struct super_block *sb, btree_handle_t *handle, __u32 node_block, __u32 key) {
  int                ret = 0;
  __u32              i, child_idx, child_block;
  struct btree_node *node = NULL, *child = NULL;

  if (node_block == 0)
    return 0;

  size_t size = handle->entry_size;

  if (btree_read_node(sb, handle, node_block, &node) != 0 || !node)
    return 0;

  if (node->node->is_leaf) {
    i = 0;
    while (i < node->node->num_entries && handle->get_key(bentries(node) + i * size) < key)
      i++;

    if (i < node->node->num_entries && handle->get_key(bentries(node) + i * size) == key) {
      btree_shift_left(bentries(node), size, i, node->node->num_entries);
      node->node->num_entries--;
      mark_buffer_dirty(node->bh);
      ret = 1;
    }

    btree_free_node(node);
    return ret;
  }

  child_idx = btree_descend_index_key(node, handle, key);

  if (btree_read_node(sb, handle, bchildren(node, handle)[child_idx], &child) != 0 || !child) {
    btree_free_node(node);
    return 0;
  }

  if (child->node->num_entries < (__u32)handle->min_degree) {
    __btree_fill(sb, handle, node_block, node, (int)child_idx);
    mark_buffer_dirty(node->bh);

    child_idx = btree_descend_index_key(node, handle, key);
    if (child_idx > node->node->num_entries)
      child_idx = node->node->num_entries;
  }

  btree_free_node(child);

  child_block = bchildren(node, handle)[child_idx];
  btree_free_node(node);

  return __btree_remove_internal(sb, handle, child_block, key);
}

/*
 * B+ removal by comparator.
 *
 * Since every record lives in a leaf, there is no predecessor/successor swap
 * like in a B-tree: we descend to the leaf making sure along the way that each
 * visited child holds at least `min_degree` entries (via __btree_fill), and the
 * deletion happens only in the leaf. Separators left stale by this are still
 * valid routing keys.
 */
static void btree_remove_entry_internal(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const void *needle,
                                        int *success) {
  int                ret;
  __u32              i, child_idx, child_block;
  __u64              size;
  __u8              *entries;
  struct btree_node *node = NULL, *child = NULL;

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

  if (node->node->is_leaf) {
    i = 0;
    while (i < node->node->num_entries && handle->cmp(entries + i * size, needle) < 0)
      i++;

    if (i < node->node->num_entries && handle->cmp(entries + i * size, needle) == 0) {
      btree_shift_left(entries, size, (int)i, (int)node->node->num_entries);
      node->node->num_entries--;
      mark_buffer_dirty(node->bh);
      *success = 1;
    } else {
      *success = 0;
    }

    btree_free_node(node);
    return;
  }

  child_idx = btree_descend_index_cmp(node, handle, needle);

  ret = btree_read_node(sb, handle, bchildren(node, handle)[child_idx], &child);
  if (ret != 0 || !child) {
    *success = 0;
    btree_free_node(node);
    return;
  }

  if (child->node->num_entries < (__u32)handle->min_degree) {
    __btree_fill(sb, handle, node_block, node, (int)child_idx);
    mark_buffer_dirty(node->bh);

    // fill() may have merged/borrowed: the separators changed, so the descent
    // index has to be recomputed against the already-updated node.
    child_idx = btree_descend_index_cmp(node, handle, needle);
    if (child_idx > node->node->num_entries)
      child_idx = node->node->num_entries;
  }

  btree_free_node(child);

  child_block = bchildren(node, handle)[child_idx];
  btree_free_node(node);

  if (child_block == 0) {
    *success = 0;
    return;
  }

  btree_remove_entry_internal(sb, handle, child_block, needle, success);
}

int btree_remove_entry(struct super_block *sb, btree_handle_t *handle, const void *entry) {
  int                success;
  void              *needle;
  struct btree_node *root;

  if (!handle || handle->root_block == 0)
    return 0;

  // callers may pass a pointer into a live tree node's buffer
  // merges and shifts during removal would rewrite it mid-recursion, so compare against a stable copy
  needle = kmemdup(entry, handle->entry_size, GFP_KERNEL);

  if (!needle)
    return 0;

  success = 0;
  btree_remove_entry_internal(sb, handle, handle->root_block, needle, &success);
  kfree(needle);

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

  // B+: in an internal node the parent separator moves down and becomes an
  // entry of the merged node; in a leaf it does NOT move down - it is only a
  // copy of the right leaf's first entry, which is already being concatenated
  // (moving it down would duplicate the record).
  if (!left->node->is_leaf) {
    memcpy(bentries(left) + left_entries * size, bentries(parent) + idx * size, size);
    left_entries++;
  }

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

  sibling_children = bchildren(sibling, handle);

  if (child->node->is_leaf) {
    // B+: the sibling's last RECORD becomes the child's first one, and the
    // parent separator becomes a copy of that new first entry (rotating through
    // the parent, as in a B-tree, would duplicate or lose a record).
    memcpy(child_entries, sibling_entries + (sibling->node->num_entries - 1) * size, size);
    memcpy(parent_entries + (idx - 1) * size, child_entries, size);
  } else {
    // Internal node: classic rotation through the parent separator.
    memcpy(child_entries, parent_entries + (idx - 1) * size, size);
    child_children[0] = sibling_children[sibling->node->num_entries];
    memcpy(parent_entries + (idx - 1) * size, sibling_entries + (sibling->node->num_entries - 1) * size, size);
  }

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

  child_children   = bchildren(child, handle);
  sibling_children = bchildren(sibling, handle);

  if (child->node->is_leaf) {
    // B+: the right sibling's first RECORD moves to the end of the child, and
    // the parent separator becomes a copy of the sibling's new first entry.
    memcpy(child_entries + child->node->num_entries * size, sibling_entries, size);
    if (sibling->node->num_entries > 1)
      memcpy(parent_entries + idx * size, sibling_entries + size, size);
  } else {
    memcpy(child_entries + child->node->num_entries * size, parent_entries + idx * size, size);
    child_children[child->node->num_entries + 1] = sibling_children[0];
    memcpy(parent_entries + idx * size, sibling_entries, size);
  }

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
