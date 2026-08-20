/*
 * recovery.bsfs - userspace tool to inspect the BSFS recovery tree.
 *
 * opens an unmounted BSFS image or block device and parses the on-disk recovery
 * B-tree of rec_entry_t records (keyed by recovery_id) defined in
 * include/bsfs_shared.h. that header is dual-mode (#ifdef __KERNEL__), so the same
 * struct layout the kernel and mkfs use gets reused here, and the whole thing runs
 * on the host with no kernel build or qemu reboot needed.
 *
 * the kernel snapshots deleted files into the recovery tree during
 * unlink -> evict_inode. this lets an operator inspect those snapshots on an
 * UNMOUNTED image.
 *
 * subcommands:
 *   recovery.bsfs <img> list [name]   list recovery entries (optional name filter)
 *   recovery.bsfs <img> show <id>     show all fields of one entry by recovery_id
 *   recovery.bsfs <img> stats         summary: count, reclaimable size, age range
 *   recovery.bsfs <img> restore <id> [absolute-destination-directory]
 *
 * restore stays offline. it copy-on-writes metadata, makes allocations durable
 * before publishing the new roots in the superblock, then releases the replaced
 * metadata. the recovered data extent tree transfers straight to the new inode;
 * its blocks are never copied or freed.
 */
#include "../include/bsfs_shared.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(bitmap_block_t) == BSFS_BLOCK_SIZE, "bitmap block layout must remain exactly one disk block");
_Static_assert(sizeof(btree_node_t) == BSFS_BLOCK_SIZE, "B+ tree node layout must remain exactly one disk block");

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, uint64_t)
#endif

/* every walk is bounded by this: a child pointer aimed at an ancestor is otherwise unbounded */
#define MAX_TREE_DEPTH 64

/* ----- low level block IO ------------------------------------------------ */

static int read_block(FILE *img, uint32_t block, void *buf) {
  if (fseeko(img, (off_t)block * BSFS_BLOCK_SIZE, SEEK_SET) != 0)
    return -1;
  if (fread(buf, BSFS_BLOCK_SIZE, 1, img) != 1)
    return -1;
  return 0;
}

static int write_block(FILE *img, uint32_t block, const void *buf) {
  if (fseeko(img, (off_t)block * BSFS_BLOCK_SIZE, SEEK_SET) != 0)
    return -1;
  if (fwrite(buf, BSFS_BLOCK_SIZE, 1, img) != 1)
    return -1;
  return 0;
}

static int sync_image(FILE *img) {
  if (fflush(img) != 0)
    return -1;
  return fsync(fileno(img));
}

static int validate_backing_size(FILE *img, const bsfs_super_block *sb) {
  struct stat st;
  uint64_t    bytes;

  if (fstat(fileno(img), &st) != 0)
    return -1;
  if (S_ISREG(st.st_mode)) {
    if (st.st_size < 0)
      return -1;
    bytes = (uint64_t)st.st_size;
  } else if (S_ISBLK(st.st_mode)) {
    if (ioctl(fileno(img), BLKGETSIZE64, &bytes) != 0)
      return -1;
  } else {
    return -1;
  }
  if (bytes < sb->fs_size || bytes < (uint64_t)sb->total_blocks * BSFS_BLOCK_SIZE) {
    fprintf(stderr, "target is smaller than the filesystem declared by its superblock\n");
    return -1;
  }
  return 0;
}

/* ----- node layout helpers (must match kernel src/btree.c) --------------- */

/* replicated from src/btree.c:compute_min_degree(), pure, no kernel deps. */
static int compute_min_degree(uint64_t entry_size) {
  int t = 2;
  while (1) {
    uint64_t entry_area    = entry_size * (2 * (uint64_t)t - 1);
    uint64_t pointer_area  = sizeof(uint32_t) * (2 * (uint64_t)t);
    uint64_t node_overhead = sizeof(uint8_t) + sizeof(uint32_t);
    if (entry_area + pointer_area + node_overhead > BSFS_BLOCK_SIZE)
      return t - 1;
    t++;
  }
}

static uint8_t *node_entries(btree_node_t *n) {
  return n->data;
}

static uint32_t *node_children(btree_node_t *n, int t, size_t entry_size) {
  size_t off = (size_t)(2 * t - 1) * entry_size;
  return (uint32_t *)(n->data + off);
}

/* ----- offline metadata transaction ------------------------------------- */

typedef uint32_t (*entry_key_fn)(const void *entry);
typedef int (*entry_cmp_fn)(const void *a, const void *b);

struct tree_desc {
  const char  *name;
  size_t       entry_size;
  int          min_degree;
  entry_key_fn key;
  entry_cmp_fn cmp;
};

struct u32_vec {
  uint32_t *items;
  size_t    count;
  size_t    cap;
};

struct bitmap_state {
  bitmap_block_t *blocks;
  uint32_t       *disk_blocks;
  uint32_t        count;
};

struct restore_txn {
  FILE                 *img;
  const bsfs_super_block *sb;
  struct bitmap_state  *bitmap;
  struct u32_vec        allocated;
  struct u32_vec        retired;
};

struct disk_node {
  uint32_t     block;
  btree_node_t raw;
};

static uint32_t inode_key(const void *entry) {
  return ((const disk_mapped_inode_t *)entry)->ino;
}

static uint32_t rec_key(const void *entry) {
  return ((const rec_entry_t *)entry)->recovery_id;
}

static uint32_t bspan_key(const void *entry) {
  return ((const bspan_t *)entry)->file_blk;
}

static uint32_t dentry_key(const void *entry) {
  return ((const bsfs_dentry *)entry)->name_hash;
}

static int cmp_u32_key(entry_key_fn key, const void *a, const void *b) {
  uint32_t ka = key(a), kb = key(b);
  return (ka > kb) - (ka < kb);
}

static int cmp_inode(const void *a, const void *b) {
  return cmp_u32_key(inode_key, a, b);
}

static int cmp_rec(const void *a, const void *b) {
  return cmp_u32_key(rec_key, a, b);
}

static int cmp_bspan(const void *a, const void *b) {
  return cmp_u32_key(bspan_key, a, b);
}

static int cmp_dentry(const void *a, const void *b) {
  const bsfs_dentry *ea = a, *eb = b;
  int                by_hash = cmp_u32_key(dentry_key, a, b);

  if (by_hash)
    return by_hash;
  return strncmp(ea->filename, eb->filename, MAX_FILENAME);
}

static struct tree_desc make_tree(const char *name, size_t entry_size, entry_key_fn key, entry_cmp_fn cmp) {
  struct tree_desc d = {
      .name       = name,
      .entry_size = entry_size,
      .min_degree = compute_min_degree(entry_size),
      .key        = key,
      .cmp        = cmp,
  };
  return d;
}

static int vec_contains(const struct u32_vec *v, uint32_t value) {
  for (size_t i = 0; i < v->count; i++)
    if (v->items[i] == value)
      return 1;
  return 0;
}

static int vec_push_unique(struct u32_vec *v, uint32_t value) {
  if (vec_contains(v, value))
    return 0;
  if (v->count == v->cap) {
    size_t    new_cap = v->cap ? v->cap * 2 : 16;
    uint32_t *grown;

    if (new_cap < v->cap || new_cap > SIZE_MAX / sizeof(*grown))
      return -1;
    grown = realloc(v->items, new_cap * sizeof(*grown));
    if (!grown)
      return -1;
    v->items = grown;
    v->cap   = new_cap;
  }
  v->items[v->count++] = value;
  return 0;
}

static void vec_remove(struct u32_vec *v, uint32_t value) {
  for (size_t i = 0; i < v->count; i++) {
    if (v->items[i] == value) {
      v->items[i] = v->items[v->count - 1];
      v->count--;
      return;
    }
  }
}

static int bitmap_index(const struct bitmap_state *bm, uint32_t block, uint32_t *map, uint32_t *bit) {
  uint32_t m = block / BITS_PER_BLOCK;

  if (m >= bm->count)
    return -1;
  *map = m;
  *bit = block % BITS_PER_BLOCK;
  return 0;
}

static int bitmap_is_set(const struct bitmap_state *bm, uint32_t block) {
  uint32_t map, bit;

  if (bitmap_index(bm, block, &map, &bit) != 0)
    return 0;
  return !!(bm->blocks[map].bits[bit >> 3] & (uint8_t)(1U << (bit & 7)));
}

static int bitmap_set(struct bitmap_state *bm, uint32_t block, int used) {
  uint32_t map, bit;
  uint8_t *byte, mask;

  if (bitmap_index(bm, block, &map, &bit) != 0)
    return -1;
  byte = &bm->blocks[map].bits[bit >> 3];
  mask = (uint8_t)(1U << (bit & 7));
  if (used)
    *byte |= mask;
  else
    *byte &= (uint8_t)~mask;
  return 0;
}

static void free_bitmap(struct bitmap_state *bm) {
  free(bm->blocks);
  free(bm->disk_blocks);
  memset(bm, 0, sizeof(*bm));
}

static int load_bitmap_state(FILE *img, const bsfs_super_block *sb, struct bitmap_state *bm) {
  struct u32_vec seen = {0};
  uint32_t       block;
  int            rc = -1;

  memset(bm, 0, sizeof(*bm));
  if (sb->block_bitmap_total == 0 ||
      sb->block_bitmap_total < (sb->total_blocks + (uint64_t)BITS_PER_BLOCK - 1) / BITS_PER_BLOCK) {
    fprintf(stderr, "invalid bitmap block count %u\n", sb->block_bitmap_total);
    return -1;
  }

  bm->blocks      = calloc(sb->block_bitmap_total, sizeof(*bm->blocks));
  bm->disk_blocks = calloc(sb->block_bitmap_total, sizeof(*bm->disk_blocks));
  if (!bm->blocks || !bm->disk_blocks)
    goto out;
  bm->count = sb->block_bitmap_total;
  block     = sb->block_bitmap_start;

  for (uint32_t i = 0; i < bm->count; i++) {
    if (block >= sb->total_blocks || vec_contains(&seen, block) || vec_push_unique(&seen, block) != 0) {
      fprintf(stderr, "invalid bitmap chain at block %u\n", block);
      goto out;
    }
    bm->disk_blocks[i] = block;
    if (read_block(img, block, &bm->blocks[i]) != 0) {
      fprintf(stderr, "failed to read bitmap block %u\n", block);
      goto out;
    }
    block = bm->blocks[i].next_block;
  }
  if (block != INVALID_BLOCK) {
    fprintf(stderr, "bitmap chain is longer than block_bitmap_total\n");
    goto out;
  }
  rc = 0;

out:
  free(seen.items);
  if (rc)
    free_bitmap(bm);
  return rc;
}

static int write_bitmap_state(FILE *img, const struct bitmap_state *bm) {
  for (uint32_t i = 0; i < bm->count; i++)
    if (write_block(img, bm->disk_blocks[i], &bm->blocks[i]) != 0)
      return -1;
  return 0;
}

static uint32_t txn_alloc_block(struct restore_txn *tx) {
  for (uint32_t block = tx->sb->data_block_start; block < tx->sb->total_blocks; block++) {
    if (!bitmap_is_set(tx->bitmap, block)) {
      if (bitmap_set(tx->bitmap, block, 1) != 0 || vec_push_unique(&tx->allocated, block) != 0) {
        (void)bitmap_set(tx->bitmap, block, 0);
        return INVALID_BLOCK;
      }
      return block;
    }
  }
  return INVALID_BLOCK;
}

static void txn_release_new(struct restore_txn *tx, uint32_t block) {
  if (!vec_contains(&tx->allocated, block))
    return;
  vec_remove(&tx->allocated, block);
  (void)bitmap_set(tx->bitmap, block, 0);
}

static int txn_retire(struct restore_txn *tx, uint32_t block) {
  // a corrupt tree root pointing into the metadata region would otherwise get the superblock or a
  // bitmap block handed back to the allocator at commit
  if (block < tx->sb->data_block_start || block >= tx->sb->total_blocks) {
    fprintf(stderr, "refusing to release block %u from outside the data region\n", block);
    return -1;
  }
  if (vec_contains(&tx->allocated, block)) {
    txn_release_new(tx, block);
    return 0;
  }
  return vec_push_unique(&tx->retired, block);
}

static void txn_destroy(struct restore_txn *tx) {
  free(tx->allocated.items);
  free(tx->retired.items);
  memset(tx, 0, sizeof(*tx));
}

static int tree_read(struct restore_txn *tx, const struct tree_desc *d, uint32_t block, struct disk_node **out) {
  struct disk_node *node;
  uint32_t          max_entries = (uint32_t)(2 * d->min_degree - 1);

  *out = NULL;
  // nodes only ever live in the data region; without the lower bound a corrupt btree_root would let
  // the superblock or a bitmap block be parsed as a node, cloned, and then freed
  if (block < tx->sb->data_block_start || block >= tx->sb->total_blocks || !bitmap_is_set(tx->bitmap, block)) {
    fprintf(stderr, "%s tree references invalid/unallocated block %u\n", d->name, block);
    return -1;
  }
  node = malloc(sizeof(*node));
  if (!node)
    return -1;
  node->block = block;
  if (read_block(tx->img, block, &node->raw) != 0) {
    fprintf(stderr, "failed to read %s node block %u\n", d->name, block);
    free(node);
    return -1;
  }
  if (node->raw.num_entries > max_entries || node->raw.is_leaf > 1) {
    fprintf(stderr, "corrupt %s node block %u (entries=%u leaf=%u)\n", d->name, block, node->raw.num_entries,
            node->raw.is_leaf);
    free(node);
    return -1;
  }
  *out = node;
  return 0;
}

static int tree_write(struct restore_txn *tx, const struct disk_node *node) {
  return write_block(tx->img, node->block, &node->raw);
}

static uint8_t *tree_entries(struct disk_node *node) {
  return node->raw.data;
}

static uint32_t *tree_children(struct disk_node *node, const struct tree_desc *d) {
  return node_children(&node->raw, d->min_degree, d->entry_size);
}

static int tree_new_node(struct restore_txn *tx, uint8_t is_leaf, struct disk_node **out) {
  struct disk_node *node = calloc(1, sizeof(*node));

  *out = NULL;
  if (!node)
    return -1;
  node->block = txn_alloc_block(tx);
  if (node->block == INVALID_BLOCK) {
    free(node);
    return -1;
  }
  node->raw.is_leaf = is_leaf;
  if (tree_write(tx, node) != 0) {
    txn_release_new(tx, node->block);
    free(node);
    return -1;
  }
  *out = node;
  return 0;
}

static int tree_make_private(struct restore_txn *tx, const struct tree_desc *d, uint32_t old_block, uint32_t *out_block) {
  struct disk_node *old = NULL, *copy = NULL;

  if (vec_contains(&tx->allocated, old_block)) {
    *out_block = old_block;
    return 0;
  }
  if (tree_read(tx, d, old_block, &old) != 0)
    return -1;
  if (tree_new_node(tx, old->raw.is_leaf, &copy) != 0)
    goto err;
  copy->raw = old->raw;
  if (tree_write(tx, copy) != 0)
    goto err_copy;
  if (txn_retire(tx, old_block) != 0)
    goto err_copy;
  *out_block = copy->block;
  free(copy);
  free(old);
  return 0;

err_copy:
  txn_release_new(tx, copy->block);
  free(copy);
err:
  free(old);
  return -1;
}

static uint32_t descend_cmp(const struct disk_node *node, const struct tree_desc *d, const void *entry) {
  const uint8_t *entries = node->raw.data;
  uint32_t       i = 0;

  while (i < node->raw.num_entries && d->cmp(entries + i * d->entry_size, entry) < 0)
    i++;
  if (i < node->raw.num_entries && d->cmp(entries + i * d->entry_size, entry) == 0)
    i++;
  return i;
}

static uint32_t descend_key(const struct disk_node *node, const struct tree_desc *d, uint32_t key) {
  const uint8_t *entries = node->raw.data;
  uint32_t       i = 0;

  while (i < node->raw.num_entries && d->key(entries + i * d->entry_size) < key)
    i++;
  if (i < node->raw.num_entries && d->key(entries + i * d->entry_size) == key)
    i++;
  return i;
}

static int tree_search_key(struct restore_txn *tx, const struct tree_desc *d, uint32_t root, uint32_t key, void *out) {
  uint32_t block = root;
  unsigned depth = 0;

  while (block) {
    struct disk_node *node;
    uint32_t          i = 0;

    if (depth++ > MAX_TREE_DEPTH) {
      fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
      return -1;
    }
    if (tree_read(tx, d, block, &node) != 0)
      return -1;
    while (i < node->raw.num_entries && d->key(tree_entries(node) + i * d->entry_size) < key)
      i++;
    if (node->raw.is_leaf) {
      int found = i < node->raw.num_entries && d->key(tree_entries(node) + i * d->entry_size) == key;
      if (found && out)
        memcpy(out, tree_entries(node) + i * d->entry_size, d->entry_size);
      free(node);
      return found;
    }
    block = tree_children(node, d)[(i < node->raw.num_entries &&
                                    d->key(tree_entries(node) + i * d->entry_size) == key)
                                       ? i + 1
                                       : i];
    free(node);
  }
  return 0;
}

static int tree_search_entry(struct restore_txn *tx, const struct tree_desc *d, uint32_t root, const void *needle, void *out) {
  uint32_t block = root;
  unsigned depth = 0;

  while (block) {
    struct disk_node *node;
    uint32_t          i = 0;

    if (depth++ > MAX_TREE_DEPTH) {
      fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
      return -1;
    }
    if (tree_read(tx, d, block, &node) != 0)
      return -1;
    while (i < node->raw.num_entries && d->cmp(tree_entries(node) + i * d->entry_size, needle) < 0)
      i++;
    if (node->raw.is_leaf) {
      int found = i < node->raw.num_entries && d->cmp(tree_entries(node) + i * d->entry_size, needle) == 0;
      if (found && out)
        memcpy(out, tree_entries(node) + i * d->entry_size, d->entry_size);
      free(node);
      return found;
    }
    if (i < node->raw.num_entries && d->cmp(tree_entries(node) + i * d->entry_size, needle) == 0)
      i++;
    block = tree_children(node, d)[i];
    free(node);
  }
  return 0;
}

static int tree_split_child(struct restore_txn *tx, const struct tree_desc *d, struct disk_node *parent, uint32_t index) {
  struct disk_node *left = NULL, *right = NULL;
  uint32_t         *children = tree_children(parent, d);
  uint8_t          *parent_entries = tree_entries(parent);
  uint32_t          left_block;
  size_t            size = d->entry_size;

  // tree_read only bounds num_entries, not the fill invariant, so a corrupt node can reach this with
  // no room for the separator; the shifts below would then write past the entry area
  if (parent->raw.num_entries >= (uint32_t)(2 * d->min_degree - 1) || index >= parent->raw.num_entries + 1) {
    fprintf(stderr, "corrupt %s node block %u (cannot split child %u of %u)\n", d->name, parent->block, index,
            parent->raw.num_entries);
    return -1;
  }
  if (tree_make_private(tx, d, children[index], &left_block) != 0)
    return -1;
  children[index] = left_block;
  if (tree_read(tx, d, left_block, &left) != 0 || tree_new_node(tx, left->raw.is_leaf, &right) != 0)
    goto err;

  right->raw.num_entries = d->min_degree - 1;
  memcpy(tree_entries(right), tree_entries(left) + (size_t)d->min_degree * size,
         (size_t)(d->min_degree - 1) * size);
  if (!left->raw.is_leaf)
    memcpy(tree_children(right, d), tree_children(left, d) + d->min_degree,
           (size_t)d->min_degree * sizeof(uint32_t));

  left->raw.num_entries = left->raw.is_leaf ? d->min_degree : d->min_degree - 1;
  memmove(&children[index + 2], &children[index + 1],
          (parent->raw.num_entries - index) * sizeof(uint32_t));
  children[index + 1] = right->block;
  memmove(parent_entries + (index + 1) * size, parent_entries + index * size,
          (parent->raw.num_entries - index) * size);
  if (left->raw.is_leaf)
    memcpy(parent_entries + index * size, tree_entries(right), size);
  else
    memcpy(parent_entries + index * size,
           tree_entries(left) + (size_t)(d->min_degree - 1) * size, size);
  parent->raw.num_entries++;

  if (tree_write(tx, left) != 0 || tree_write(tx, right) != 0 || tree_write(tx, parent) != 0)
    goto err;
  free(left);
  free(right);
  return 0;

err:
  free(left);
  free(right);
  return -1;
}

static int tree_insert_nonfull(struct restore_txn *tx, const struct tree_desc *d, uint32_t block, const void *entry,
                               unsigned depth) {
  struct disk_node *node = NULL, *child = NULL;
  uint8_t          *entries;
  uint32_t          i;
  size_t            size = d->entry_size;
  int               rc = -1;

  if (depth > MAX_TREE_DEPTH) {
    fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  entries = tree_entries(node);
  if (node->raw.is_leaf) {
    // the caller's pre-emptive split guarantees room, but a corrupt leaf can arrive already full and
    // the shift below would run off the end of the entry area
    if (node->raw.num_entries >= (uint32_t)(2 * d->min_degree - 1)) {
      fprintf(stderr, "corrupt %s node block %u (insert into a full leaf)\n", d->name, block);
      goto out;
    }
    i = node->raw.num_entries;
    while (i > 0 && d->cmp(entries + (i - 1) * size, entry) > 0)
      i--;
    memmove(entries + (i + 1) * size, entries + i * size,
            (node->raw.num_entries - i) * size);
    memcpy(entries + i * size, entry, size);
    node->raw.num_entries++;
    rc = tree_write(tx, node);
    goto out;
  }

  i = descend_cmp(node, d, entry);
  {
    uint32_t private_child;
    if (tree_make_private(tx, d, tree_children(node, d)[i], &private_child) != 0)
      goto out;
    tree_children(node, d)[i] = private_child;
    if (tree_write(tx, node) != 0 || tree_read(tx, d, private_child, &child) != 0)
      goto out;
  }
  if (child->raw.num_entries == (uint32_t)(2 * d->min_degree - 1)) {
    free(child);
    child = NULL;
    if (tree_split_child(tx, d, node, i) != 0)
      goto out;
    if (d->cmp(entry, tree_entries(node) + i * size) >= 0)
      i++;
  }
  free(child);
  child = NULL;
  rc = tree_insert_nonfull(tx, d, tree_children(node, d)[i], entry, depth + 1);

out:
  free(child);
  free(node);
  return rc;
}

static int tree_insert(struct restore_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry) {
  struct disk_node *node = NULL, *new_root = NULL;
  uint32_t          private_root;
  int               found;

  found = tree_search_entry(tx, d, *root, entry, NULL);
  if (found != 0)
    return -1;
  if (*root == 0) {
    if (tree_new_node(tx, 1, &node) != 0)
      return -1;
    *root = node->block;
    free(node);
    return tree_insert_nonfull(tx, d, *root, entry, 0);
  }
  if (tree_make_private(tx, d, *root, &private_root) != 0)
    return -1;
  *root = private_root;
  if (tree_read(tx, d, *root, &node) != 0)
    return -1;
  if (node->raw.num_entries == (uint32_t)(2 * d->min_degree - 1)) {
    if (tree_new_node(tx, 0, &new_root) != 0)
      goto err;
    tree_children(new_root, d)[0] = *root;
    if (tree_write(tx, new_root) != 0 || tree_split_child(tx, d, new_root, 0) != 0)
      goto err;
    *root = new_root->block;
  }
  free(node);
  free(new_root);
  return tree_insert_nonfull(tx, d, *root, entry, 0);

err:
  free(node);
  free(new_root);
  return -1;
}

static int tree_update_key_rec(struct restore_txn *tx, const struct tree_desc *d, uint32_t block, const void *entry,
                               unsigned depth) {
  struct disk_node *node = NULL;
  uint32_t          key = d->key(entry), i = 0;
  int               rc = -1;

  if (depth > MAX_TREE_DEPTH) {
    fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  while (i < node->raw.num_entries && d->key(tree_entries(node) + i * d->entry_size) < key)
    i++;
  if (node->raw.is_leaf) {
    if (i >= node->raw.num_entries || d->key(tree_entries(node) + i * d->entry_size) != key)
      goto out;
    memcpy(tree_entries(node) + i * d->entry_size, entry, d->entry_size);
    rc = tree_write(tx, node);
  } else {
    uint32_t idx = descend_key(node, d, key), child;
    if (tree_make_private(tx, d, tree_children(node, d)[idx], &child) != 0)
      goto out;
    tree_children(node, d)[idx] = child;
    if (tree_write(tx, node) != 0)
      goto out;
    rc = tree_update_key_rec(tx, d, child, entry, depth + 1);
  }

out:
  free(node);
  return rc;
}

static int tree_update_key(struct restore_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry) {
  uint32_t private_root;

  if (*root == 0 || tree_make_private(tx, d, *root, &private_root) != 0)
    return -1;
  *root = private_root;
  return tree_update_key_rec(tx, d, *root, entry, 0);
}

struct entry_collection {
  uint8_t       *entries;
  size_t         count;
  size_t         cap;
  struct u32_vec nodes;
};

static int collection_add(struct entry_collection *c, const struct tree_desc *d, const void *entry) {
  if (c->count == c->cap) {
    size_t   new_cap = c->cap ? c->cap * 2 : 32;
    uint8_t *grown;

    if (new_cap < c->cap || new_cap > SIZE_MAX / d->entry_size)
      return -1;
    grown = realloc(c->entries, new_cap * d->entry_size);
    if (!grown)
      return -1;
    c->entries = grown;
    c->cap     = new_cap;
  }
  if (c->count && d->cmp(c->entries + (c->count - 1) * d->entry_size, entry) >= 0) {
    fprintf(stderr, "%s tree leaf records are not strictly ordered\n", d->name);
    return -1;
  }
  memcpy(c->entries + c->count * d->entry_size, entry, d->entry_size);
  c->count++;
  return 0;
}

static int collect_leaf_entries(struct restore_txn *tx, const struct tree_desc *d, uint32_t block,
                                struct entry_collection *c, unsigned depth) {
  struct disk_node *node = NULL;
  int               rc = -1;

  if (depth > MAX_TREE_DEPTH || vec_contains(&c->nodes, block) || vec_push_unique(&c->nodes, block) != 0) {
    fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  if (node->raw.is_leaf) {
    for (uint32_t i = 0; i < node->raw.num_entries; i++)
      if (collection_add(c, d, tree_entries(node) + i * d->entry_size) != 0)
        goto out;
  } else {
    for (uint32_t i = 0; i <= node->raw.num_entries; i++)
      if (collect_leaf_entries(tx, d, tree_children(node, d)[i], c, depth + 1) != 0)
        goto out;
  }
  rc = 0;

out:
  free(node);
  return rc;
}

struct level_item {
  uint32_t block;
  uint8_t *first;
};

static void free_level(struct level_item *items, size_t count) {
  if (!items)
    return;
  for (size_t i = 0; i < count; i++)
    free(items[i].first);
  free(items);
}

static int level_set_first(struct level_item *item, const void *entry, size_t size) {
  item->first = malloc(size);
  if (!item->first)
    return -1;
  memcpy(item->first, entry, size);
  return 0;
}

static int tree_bulk_build(struct restore_txn *tx, const struct tree_desc *d, const uint8_t *entries, size_t count,
                           uint32_t *out_root) {
  struct level_item *level = NULL;
  size_t             level_count, offset = 0;
  size_t             max_entries = (size_t)(2 * d->min_degree - 1);

  *out_root = 0;
  if (!count)
    return 0;
  level_count = (count + max_entries - 1) / max_entries;
  level       = calloc(level_count, sizeof(*level));
  if (!level)
    return -1;

  for (size_t i = 0; i < level_count; i++) {
    struct disk_node *leaf;
    size_t            remaining = count - offset;
    size_t            groups = level_count - i;
    size_t            n = remaining / groups + !!(remaining % groups);

    if (tree_new_node(tx, 1, &leaf) != 0)
      goto err;
    leaf->raw.num_entries = (uint32_t)n;
    memcpy(tree_entries(leaf), entries + offset * d->entry_size, n * d->entry_size);
    level[i].block = leaf->block;
    if (level_set_first(&level[i], entries + offset * d->entry_size, d->entry_size) != 0 || tree_write(tx, leaf) != 0) {
      free(leaf);
      goto err;
    }
    free(leaf);
    offset += n;
  }

  while (level_count > 1) {
    size_t             max_children = (size_t)(2 * d->min_degree);
    size_t             parent_count = (level_count + max_children - 1) / max_children;
    struct level_item *parents = calloc(parent_count, sizeof(*parents));
    size_t             child_offset = 0;

    if (!parents)
      goto err;
    for (size_t p = 0; p < parent_count; p++) {
      struct disk_node *parent;
      size_t            remaining = level_count - child_offset;
      size_t            groups = parent_count - p;
      size_t            children = remaining / groups + !!(remaining % groups);

      if (tree_new_node(tx, 0, &parent) != 0) {
        free_level(parents, parent_count);
        goto err;
      }
      parent->raw.num_entries = (uint32_t)(children - 1);
      for (size_t j = 0; j < children; j++) {
        tree_children(parent, d)[j] = level[child_offset + j].block;
        if (j)
          memcpy(tree_entries(parent) + (j - 1) * d->entry_size, level[child_offset + j].first, d->entry_size);
      }
      parents[p].block = parent->block;
      if (level_set_first(&parents[p], level[child_offset].first, d->entry_size) != 0 || tree_write(tx, parent) != 0) {
        free(parent);
        free_level(parents, parent_count);
        goto err;
      }
      free(parent);
      child_offset += children;
    }
    free_level(level, level_count);
    level       = parents;
    level_count = parent_count;
  }

  *out_root = level[0].block;
  free_level(level, level_count);
  return 0;

err:
  free_level(level, level_count);
  return -1;
}

static int rebuild_without_key(struct restore_txn *tx, const struct tree_desc *d, uint32_t old_root, uint32_t remove_key,
                               uint32_t *new_root) {
  struct entry_collection c = {0};
  uint8_t                *filtered = NULL;
  size_t                  kept = 0, removed = 0;
  int                     rc = -1;

  if (!old_root || collect_leaf_entries(tx, d, old_root, &c, 0) != 0)
    goto out;
  filtered = malloc(c.count * d->entry_size);
  if (c.count && !filtered)
    goto out;
  for (size_t i = 0; i < c.count; i++) {
    void *entry = c.entries + i * d->entry_size;
    if (d->key(entry) == remove_key) {
      removed++;
      continue;
    }
    memcpy(filtered + kept++ * d->entry_size, entry, d->entry_size);
  }
  if (removed != 1) {
    fprintf(stderr, "recovery id %u is missing or duplicated\n", remove_key);
    goto out;
  }
  if (tree_bulk_build(tx, d, filtered, kept, new_root) != 0)
    goto out;
  for (size_t i = 0; i < c.nodes.count; i++)
    if (txn_retire(tx, c.nodes.items[i]) != 0)
      goto out;
  rc = 0;

out:
  free(filtered);
  free(c.entries);
  free(c.nodes.items);
  return rc;
}

/* returns 0 on success, -1 before publication, -2 if publication is uncertain. */
static int txn_commit(struct restore_txn *tx, const bsfs_super_block *new_sb) {
  uint8_t super_raw[BSFS_BLOCK_SIZE];

  if (sync_image(tx->img) != 0) {
    perror("sync shadow metadata");
    return -1;
  }
  if (write_bitmap_state(tx->img, tx->bitmap) != 0 || sync_image(tx->img) != 0) {
    perror("persist allocated bitmap blocks");
    return -1;
  }
  if (read_block(tx->img, 1, super_raw) != 0) {
    fprintf(stderr, "failed to reread superblock before commit\n");
    return -1;
  }
  memcpy(super_raw, new_sb, sizeof(*new_sb));
  if (write_block(tx->img, 1, super_raw) != 0 || sync_image(tx->img) != 0) {
    fprintf(stderr, "superblock publication may be incomplete; inspect the image before retrying\n");
    return -2;
  }

  for (size_t i = 0; i < tx->retired.count; i++)
    (void)bitmap_set(tx->bitmap, tx->retired.items[i], 0);
  if (write_bitmap_state(tx->img, tx->bitmap) != 0 || sync_image(tx->img) != 0)
    fprintf(stderr, "warning: restored file is committed, but obsolete metadata blocks may remain allocated\n");
  return 0;
}

/* ----- formatting -------------------------------------------------------- */

/* format a POSIX timestamp as "YYYY-mm-dd HH:MM:SS", or "-" when zero. */
static void fmt_time(uint64_t ts, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  if (ts == 0) {
    snprintf(buf, cap, "-");
    return;
  }
  time_t    t = (time_t)ts;
  struct tm tm;
  // a garbage timestamp off a damaged image makes localtime_r fail, which would leave tm uninitialised
  if (!localtime_r(&t, &tm) || strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm) == 0)
    snprintf(buf, cap, "?");
}

/* file_type uses the kernel's live convention (src/inode.c): 0=DIR, 1=FILE. */
static const char *type_str(uint8_t file_type) {
  return file_type == 0 ? "DIR" : "FILE";
}

/* original_name comes straight off disk and a damaged record need not be NUL-terminated */
static int name_len(const rec_entry_t *e) {
  return (int)strnlen(e->original_name, sizeof(e->original_name));
}

/* ----- recovery tree traversal ------------------------------------------- */

/*
 * visitor invoked once per recovery entry, in ascending recovery_id order.
 * return nonzero to stop the traversal early (used by 'show').
 */
typedef int (*rec_visit_fn)(const rec_entry_t *e, void *ctx);

/*
 * recursive in-order walk of the recovery B-tree rooted at @block. returns 0
 * on success (full traversal or visitor-requested stop), -1 on a read error.
 */
static int walk_rec(FILE *img, const bsfs_super_block *sb, uint32_t block, int t, rec_visit_fn visit, void *ctx,
                    unsigned depth) {
  if (block == 0)
    return 0;
  if (block < sb->data_block_start || block >= sb->total_blocks) {
    fprintf(stderr, "recovery node block %u out of range (data starts at %u, total_blocks=%u)\n", block, sb->data_block_start,
            sb->total_blocks);
    return -1;
  }
  if (depth > MAX_TREE_DEPTH) {
    fprintf(stderr, "recovery tree contains a cycle or is too deep\n");
    return -1;
  }

  uint8_t       raw[BSFS_BLOCK_SIZE];
  btree_node_t *n = (btree_node_t *)raw;
  if (read_block(img, block, raw) != 0) {
    fprintf(stderr, "failed to read recovery node block %u\n", block);
    return -1;
  }

  uint32_t maxn = (uint32_t)(2 * t - 1);
  if (n->num_entries > maxn || n->is_leaf > 1) {
    fprintf(stderr, "recovery node block %u is corrupt (entries=%u max=%u leaf=%u)\n", block, n->num_entries, maxn, n->is_leaf);
    return -1;
  }

  rec_entry_t *ents = (rec_entry_t *)node_entries(n);
  uint32_t    *kids = node_children(n, t, sizeof(rec_entry_t));

  if (n->is_leaf) {
    for (uint32_t i = 0; i < n->num_entries; i++)
      if (visit(&ents[i], ctx))
        return 0; /* visitor asked to stop */
    return 0;
  }

  /* internal entries are B+ separator copies, not authoritative records. */
  for (uint32_t i = 0; i <= n->num_entries; i++)
    if (walk_rec(img, sb, kids[i], t, visit, ctx, depth + 1) != 0)
      return -1;
  return 0;
}

/* ----- subcommands ------------------------------------------------------- */

static int load_super(FILE *img, bsfs_super_block *sb) {
  uint8_t raw[BSFS_BLOCK_SIZE];
  if (read_block(img, 1, raw) != 0) {
    fprintf(stderr, "failed to read superblock\n");
    return -1;
  }
  memcpy(sb, raw, sizeof(*sb));
  if (sb->magic_number != MAGIC_NUMBER) {
    fprintf(stderr, "bad magic 0x%08x (expected 0x%08x)\n", sb->magic_number, MAGIC_NUMBER);
    return -1;
  }
  if (sb->block_size != BSFS_BLOCK_SIZE || sb->total_blocks < 4 ||
      sb->fs_size != (uint64_t)sb->total_blocks * BSFS_BLOCK_SIZE || sb->block_bitmap_start <= 1 ||
      (uint64_t)sb->block_bitmap_start + sb->block_bitmap_total > sb->data_block_start ||
      sb->data_block_start >= sb->total_blocks || sb->inode_root < sb->data_block_start || sb->inode_root >= sb->total_blocks ||
      (sb->recovery_root && (sb->recovery_root < sb->data_block_start || sb->recovery_root >= sb->total_blocks)) ||
      sb->root_inode == 0) {
    fprintf(stderr, "invalid BSFS superblock geometry\n");
    return -1;
  }
  return 0;
}

/* --- list --- */

struct list_ctx {
  const char *filter; /* exact original_name match, or NULL for all */
  size_t      printed;
};

static int visit_list(const rec_entry_t *e, void *vctx) {
  struct list_ctx *c = vctx;
  int              len = name_len(e);

  if (c->filter && (strlen(c->filter) != (size_t)len || memcmp(e->original_name, c->filter, (size_t)len) != 0))
    return 0;
  char delbuf[32], expbuf[32];
  fmt_time(e->deleted_at, delbuf, sizeof(delbuf));
  fmt_time(e->retention_until, expbuf, sizeof(expbuf));
  printf("%-6u %-5s %-8u %-19s %-19s %-10u %.*s\n", e->recovery_id, type_str(e->file_type), e->file_size, delbuf, expbuf,
         e->original_parent_ino, len, e->original_name);
  c->printed++;
  return 0;
}

static int cmd_list(FILE *img, const bsfs_super_block *sb, const char *filter) {
  if (sb->recovery_root == 0) {
    printf("No recovery entries.\n");
    return 0;
  }

  printf("Recovery entries%s:\n", filter ? " (filtered)" : "");
  printf("%-6s %-5s %-8s %-19s %-19s %-10s %s\n", "ID", "Type", "Size", "Deleted At", "Retain Until", "ParentIno", "Name");
  printf("%-6s %-5s %-8s %-19s %-19s %-10s %s\n", "--", "----", "----", "-------------------", "-------------------", "----------", "----");

  struct list_ctx c = {.filter = filter, .printed = 0};
  int             t = compute_min_degree(sizeof(rec_entry_t));
  if (walk_rec(img, sb, sb->recovery_root, t, visit_list, &c, 0) != 0) {
    fprintf(stderr, "error reading recovery entries\n");
    return 2;
  }

  if (c.printed == 0) {
    if (filter)
      printf("No recovery entries match name '%s'.\n", filter);
    else
      printf("No recovery entries.\n");
  }
  return 0;
}

/* --- show --- */

struct show_ctx {
  uint32_t    id;
  int         found;
  rec_entry_t entry;
};

static int visit_show(const rec_entry_t *e, void *vctx) {
  struct show_ctx *c = vctx;
  if (e->recovery_id == c->id) {
    c->entry = *e;
    c->found = 1;
    return 1; /* stop traversal */
  }
  return 0;
}

static int cmd_show(FILE *img, const bsfs_super_block *sb, uint32_t id) {
  struct show_ctx c = {.id = id, .found = 0};
  if (sb->recovery_root != 0) {
    int t = compute_min_degree(sizeof(rec_entry_t));
    if (walk_rec(img, sb, sb->recovery_root, t, visit_show, &c, 0) != 0) {
      fprintf(stderr, "error reading recovery entries\n");
      return 2;
    }
  }
  if (!c.found) {
    fprintf(stderr, "recovery id %u not found\n", id);
    return 2;
  }

  const rec_entry_t *e = &c.entry;
  char               delbuf[32], expbuf[32];
  fmt_time(e->deleted_at, delbuf, sizeof(delbuf));
  fmt_time(e->retention_until, expbuf, sizeof(expbuf));

  printf("Recovery entry %u:\n", e->recovery_id);
  printf("  original_name        %.*s\n", name_len(e), e->original_name);
  printf("  type                 %s\n", type_str(e->file_type));
  printf("  original_parent_ino  %u\n", e->original_parent_ino);
  printf("  inode_number         %u\n", e->inode_number);
  printf("  file_size            %u\n", e->file_size);
  printf("  block_count          %u\n", e->block_count);
  printf("  uid / gid            %u / %u\n", e->uid, e->gid);
  printf("  permissions          0%o\n", e->permissions);
  printf("  deleted_at           %s\n", delbuf);
  printf("  retention_until      %s\n", expbuf);
  printf("  btree_root           %u\n", e->btree_root);
  return 0;
}

/* --- restore --- */

struct extent_check {
  struct u32_vec nodes;
  uint64_t       logical_end;
  uint32_t       file_blocks;
  int            have_span;
};

static int validate_extent_node(struct restore_txn *tx, const struct tree_desc *d, uint32_t block,
                                struct extent_check *check, unsigned depth) {
  struct disk_node *node = NULL;
  int               rc = -1;

  if (depth > MAX_TREE_DEPTH || vec_contains(&check->nodes, block) || vec_push_unique(&check->nodes, block) != 0) {
    fprintf(stderr, "extent tree contains a cycle or is too deep\n");
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  if (!node->raw.is_leaf) {
    for (uint32_t i = 0; i <= node->raw.num_entries; i++)
      if (validate_extent_node(tx, d, tree_children(node, d)[i], check, depth + 1) != 0)
        goto out;
  } else {
    for (uint32_t i = 0; i < node->raw.num_entries; i++) {
      const bspan_t *span = (const bspan_t *)(tree_entries(node) + i * d->entry_size);
      uint64_t       logical_end = (uint64_t)span->file_blk + span->length;
      uint64_t       disk_end = (uint64_t)span->disk_blk + span->length;

      if (!span->length || logical_end > check->file_blocks || disk_end > tx->sb->total_blocks ||
          span->disk_blk < tx->sb->data_block_start || (check->have_span && span->file_blk < check->logical_end)) {
        fprintf(stderr, "invalid or overlapping extent at logical block %u\n", span->file_blk);
        goto out;
      }
      for (uint32_t j = 0; j < span->length; j++) {
        if (!bitmap_is_set(tx->bitmap, span->disk_blk + j)) {
          fprintf(stderr, "extent references free data block %u\n", span->disk_blk + j);
          goto out;
        }
      }
      check->logical_end = logical_end;
      check->have_span   = 1;
    }
  }
  rc = 0;

out:
  free(node);
  return rc;
}

static int validate_recovery_file(struct restore_txn *tx, const struct tree_desc *bspan_tree, const rec_entry_t *rec) {
  struct extent_check check = {
      .file_blocks = (rec->file_size + (uint64_t)BSFS_BLOCK_SIZE - 1) / BSFS_BLOCK_SIZE,
  };
  size_t name_len = strnlen(rec->original_name, sizeof(rec->original_name));
  int    rc = -1;

  if (rec->file_type != 1 || !S_ISREG((mode_t)rec->permissions)) {
    fprintf(stderr, "recovery id %u is not a regular file\n", rec->recovery_id);
    return -1;
  }
  if (!name_len || name_len >= MAX_FILENAME || strchr(rec->original_name, '/') || !strcmp(rec->original_name, ".") ||
      !strcmp(rec->original_name, "..")) {
    fprintf(stderr, "recovery id %u has an invalid original name\n", rec->recovery_id);
    return -1;
  }
  if (!rec->btree_root)
    return 0; /* a non-empty sparse file may legitimately have no extents. */
  rc = validate_extent_node(tx, bspan_tree, rec->btree_root, &check, 0);
  free(check.nodes.items);
  return rc;
}

static int load_inode_entry(struct restore_txn *tx, const struct tree_desc *inode_tree, uint32_t root, uint32_t ino,
                            disk_mapped_inode_t *out) {
  int found = tree_search_key(tx, inode_tree, root, ino, out);

  if (found < 0)
    return -1;
  return found;
}

static int inode_is_live_dir(const disk_mapped_inode_t *inode) {
  return inode->nlinks != 0 && S_ISDIR((mode_t)inode->mode);
}

static int resolve_destination_path(struct restore_txn *tx, const struct tree_desc *inode_tree,
                                    const struct tree_desc *dir_tree, uint32_t inode_root, const char *path,
                                    disk_mapped_inode_t *out) {
  disk_mapped_inode_t current;
  char               *copy = NULL, *save = NULL, *component;
  int                 rc = -1;

  if (!path || path[0] != '/') {
    fprintf(stderr, "destination must be an absolute BSFS directory path\n");
    return -1;
  }
  if (load_inode_entry(tx, inode_tree, inode_root, tx->sb->root_inode, &current) != 1 || !inode_is_live_dir(&current)) {
    fprintf(stderr, "BSFS root inode is missing or invalid\n");
    return -1;
  }
  copy = strdup(path);
  if (!copy)
    return -1;
  component = strtok_r(copy, "/", &save);
  while (component) {
    bsfs_dentry needle = {0}, found;
    size_t      len = strlen(component);

    if (!len || len >= MAX_FILENAME || !strcmp(component, ".") || !strcmp(component, "..")) {
      fprintf(stderr, "destination contains an invalid path component\n");
      goto out;
    }
    memcpy(needle.filename, component, len + 1);
    needle.name_hash = hash_filename(needle.filename);
    if (tree_search_entry(tx, dir_tree, current.btree_root, &needle, &found) != 1 ||
        load_inode_entry(tx, inode_tree, inode_root, found.ino, &current) != 1 || !inode_is_live_dir(&current)) {
      fprintf(stderr, "destination directory '%s' does not exist\n", path);
      goto out;
    }
    component = strtok_r(NULL, "/", &save);
  }
  *out = current;
  rc   = 0;

out:
  free(copy);
  return rc;
}

static int resolve_destination(struct restore_txn *tx, const struct tree_desc *inode_tree, const struct tree_desc *dir_tree,
                               uint32_t inode_root, const rec_entry_t *rec, const char *path, disk_mapped_inode_t *out) {
  if (path)
    return resolve_destination_path(tx, inode_tree, dir_tree, inode_root, path, out);
  if (rec->original_parent_ino &&
      load_inode_entry(tx, inode_tree, inode_root, rec->original_parent_ino, out) == 1 && inode_is_live_dir(out))
    return 0;
  if (load_inode_entry(tx, inode_tree, inode_root, tx->sb->root_inode, out) == 1 && inode_is_live_dir(out))
    return 0;
  fprintf(stderr, "neither the original parent nor the BSFS root is usable\n");
  return -1;
}

static int find_max_inode(struct restore_txn *tx, const struct tree_desc *inode_tree, uint32_t root, uint32_t *out) {
  uint32_t block = root;
  unsigned depth = 0;

  *out = 0;
  while (block) {
    struct disk_node *node;
    if (depth++ > MAX_TREE_DEPTH) {
      fprintf(stderr, "%s tree contains a cycle or is too deep\n", inode_tree->name);
      return -1;
    }
    if (tree_read(tx, inode_tree, block, &node) != 0)
      return -1;
    if (node->raw.is_leaf) {
      if (node->raw.num_entries)
        *out = inode_key(tree_entries(node) + (node->raw.num_entries - 1) * inode_tree->entry_size);
      free(node);
      return 0;
    }
    block = tree_children(node, inode_tree)[node->raw.num_entries];
    free(node);
  }
  return 0;
}

static int cmd_restore(FILE *img, const bsfs_super_block *sb, uint32_t id, const char *destination) {
  struct bitmap_state bitmap = {0};
  struct restore_txn  tx = {.img = img, .sb = sb, .bitmap = &bitmap};
  struct tree_desc    inode_tree = make_tree("inode", sizeof(disk_mapped_inode_t), inode_key, cmp_inode);
  struct tree_desc    dir_tree = make_tree("directory", sizeof(bsfs_dentry), dentry_key, cmp_dentry);
  struct tree_desc    rec_tree = make_tree("recovery", sizeof(rec_entry_t), rec_key, cmp_rec);
  struct tree_desc    bspan_tree = make_tree("extent", sizeof(bspan_t), bspan_key, cmp_bspan);
  disk_mapped_inode_t parent, restored = {0}, existing;
  bsfs_dentry         dentry = {0}, collision;
  bsfs_super_block    new_sb = *sb;
  rec_entry_t         rec;
  uint32_t            inode_root = sb->inode_root, dir_root, recovery_root, ino, max_ino;
  uint32_t            now = (uint32_t)time(NULL);
  int                 found, commit_rc, rc = 2;

  if (!id) {
    fprintf(stderr, "recovery id must be nonzero\n");
    return 2;
  }
  if (load_bitmap_state(img, sb, &bitmap) != 0)
    goto out;
  found = tree_search_key(&tx, &rec_tree, sb->recovery_root, id, &rec);
  if (found != 1) {
    if (!found)
      fprintf(stderr, "recovery id %u not found\n", id);
    goto out;
  }
  if (validate_recovery_file(&tx, &bspan_tree, &rec) != 0 ||
      resolve_destination(&tx, &inode_tree, &dir_tree, inode_root, &rec, destination, &parent) != 0)
    goto out;

  memcpy(dentry.filename, rec.original_name, strlen(rec.original_name) + 1);
  dentry.name_hash = hash_filename(dentry.filename);
  found = tree_search_entry(&tx, &dir_tree, parent.btree_root, &dentry, &collision);
  if (found != 0) {
    if (found > 0)
      fprintf(stderr, "destination already contains '%s'\n", rec.original_name);
    goto out;
  }

  ino = rec.inode_number;
  found = ino ? load_inode_entry(&tx, &inode_tree, inode_root, ino, &existing) : 1;
  if (found < 0)
    goto out;
  if (!ino || found == 1) {
    if (find_max_inode(&tx, &inode_tree, inode_root, &max_ino) != 0 || max_ino == UINT32_MAX) {
      fprintf(stderr, "no inode number is available for restoration\n");
      goto out;
    }
    ino = max_ino + 1;
  }

  dentry.ino = ino;
  dir_root   = parent.btree_root;
  if (tree_insert(&tx, &dir_tree, &dir_root, &dentry) != 0) {
    fprintf(stderr, "failed to add restored directory entry (metadata space exhausted?)\n");
    goto out;
  }
  parent.btree_root = dir_root;
  parent.mtime = parent.ctime = now;
  if (tree_update_key(&tx, &inode_tree, &inode_root, &parent) != 0) {
    fprintf(stderr, "failed to update destination directory inode\n");
    goto out;
  }

  restored.ino         = ino;
  restored.size        = rec.file_size;
  restored.block_count = rec.block_count;
  restored.uid         = rec.uid;
  restored.gid         = rec.gid;
  restored.mode        = rec.permissions;
  restored.nlinks      = 1;
  restored.atime       = now;
  restored.mtime       = now;
  restored.ctime       = now;
  restored.btree_root  = rec.btree_root;
  if (tree_insert(&tx, &inode_tree, &inode_root, &restored) != 0) {
    fprintf(stderr, "failed to insert restored inode (metadata space exhausted?)\n");
    goto out;
  }
  if (rebuild_without_key(&tx, &rec_tree, sb->recovery_root, id, &recovery_root) != 0) {
    fprintf(stderr, "failed to remove recovery entry (metadata space exhausted?)\n");
    goto out;
  }

  new_sb.inode_root    = inode_root;
  new_sb.recovery_root = recovery_root;
  commit_rc            = txn_commit(&tx, &new_sb);
  if (commit_rc != 0)
    goto out;

  if (destination)
    printf("Restored recovery entry %u as %s/%s (inode %u).\n", id, !strcmp(destination, "/") ? "" : destination,
           rec.original_name, ino);
  else
    printf("Restored recovery entry %u as '%s' in parent inode %u (inode %u).\n", id, rec.original_name, parent.ino, ino);
  rc = 0;

out:
  txn_destroy(&tx);
  free_bitmap(&bitmap);
  return rc;
}

/* --- stats --- */

struct stats_ctx {
  size_t   count;
  uint64_t total_size;
  uint64_t oldest;
  uint64_t newest;
};

static int visit_stats(const rec_entry_t *e, void *vctx) {
  struct stats_ctx *c = vctx;
  if (c->count == 0 || e->deleted_at < c->oldest)
    c->oldest = e->deleted_at;
  if (c->count == 0 || e->deleted_at > c->newest)
    c->newest = e->deleted_at;
  c->total_size += e->file_size;
  c->count++;
  return 0;
}

static int cmd_stats(FILE *img, const bsfs_super_block *sb) {
  struct stats_ctx c = {0};
  if (sb->recovery_root != 0) {
    int t = compute_min_degree(sizeof(rec_entry_t));
    if (walk_rec(img, sb, sb->recovery_root, t, visit_stats, &c, 0) != 0) {
      fprintf(stderr, "error reading recovery entries\n");
      return 2;
    }
  }

  char oldbuf[32], newbuf[32];
  fmt_time(c.count ? c.oldest : 0, oldbuf, sizeof(oldbuf));
  fmt_time(c.count ? c.newest : 0, newbuf, sizeof(newbuf));

  printf("Recovery tree stats:\n");
  printf("  entries          %zu\n", c.count);
  printf("  reclaimable size %" PRIu64 " bytes\n", c.total_size);
  printf("  oldest deleted   %s\n", oldbuf);
  printf("  newest deleted   %s\n", newbuf);
  return 0;
}

/* ----- main -------------------------------------------------------------- */

static void usage(const char *p) {
  fprintf(stderr,
          "Usage:\n"
          "  %s <img> list [name]\n"
          "  %s <img> show <id>\n"
          "  %s <img> stats\n"
          "  %s <img> restore <id> [absolute-bsfs-directory]\n",
          p, p, p, p);
}

static int parse_id(const char *text, uint32_t *out) {
  char         *end;
  unsigned long value;

  errno = 0;
  value = strtoul(text, &end, 0);
  if (errno || !text[0] || *end || value == 0 || value > UINT32_MAX)
    return -1;
  *out = (uint32_t)value;
  return 0;
}

/*
 * O_EXCL keeps us off a mounted block device, but an image file is mounted through a loop device and
 * neither O_EXCL nor flock() sees that - the loop driver just holds the file open. So walk the mount
 * table, and for every mounted device sysfs describes as loop-backed compare its backing file with
 * the target. Attached-but-unmounted is fine and stays allowed.
 *
 * Sets *checked to 0 when the mount table could not be read, i.e. the answer is "don't know".
 */
static int image_is_mounted_via_loop(const char *path, int *checked) {
  struct stat target, backing;
  char        line[4096];
  FILE       *mounts;
  int         found = 0;

  *checked = 0;
  if (stat(path, &target) != 0)
    return 0;
  mounts = fopen("/proc/self/mountinfo", "r");
  if (!mounts)
    return 0;
  *checked = 1;

  while (!found && fgets(line, sizeof(line), mounts)) {
    char  devno[64], attr[128], backing_path[PATH_MAX];
    FILE *sysfs;

    /* mountinfo field 3 is the mounted device's major:minor */
    if (sscanf(line, "%*d %*d %63s", devno) != 1)
      continue;
    if (snprintf(attr, sizeof(attr), "/sys/dev/block/%s/loop/backing_file", devno) >= (int)sizeof(attr))
      continue;
    sysfs = fopen(attr, "r");
    if (!sysfs)
      continue; /* only loop devices expose loop/backing_file */
    if (fgets(backing_path, sizeof(backing_path), sysfs)) {
      backing_path[strcspn(backing_path, "\n")] = '\0';
      found = stat(backing_path, &backing) == 0 && backing.st_dev == target.st_dev && backing.st_ino == target.st_ino;
    }
    fclose(sysfs);
  }

  fclose(mounts);
  return found;
}

static FILE *open_restore_image(const char *path) {
  struct stat st;
  int         fd, flags = O_RDWR | O_CLOEXEC;
  FILE       *img;

  if (stat(path, &st) != 0) {
    perror("stat image");
    return NULL;
  }
  if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
    fprintf(stderr, "restore target must be a regular image or block device\n");
    return NULL;
  }
  if (S_ISBLK(st.st_mode)) {
    flags |= O_EXCL;
  } else {
    int checked;

    if (image_is_mounted_via_loop(path, &checked)) {
      fprintf(stderr, "refusing to restore: %s is mounted through a loop device, unmount it first\n", path);
      return NULL;
    }
    if (!checked)
      fprintf(stderr, "warning: cannot read /proc/self/mountinfo, unable to confirm %s is not mounted\n", path);
  }
  fd = open(path, flags);
  if (fd < 0) {
    perror("open image for exclusive restore");
    return NULL;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    perror("lock image for restore");
    close(fd);
    return NULL;
  }
  img = fdopen(fd, "r+b");
  if (!img) {
    perror("fdopen image");
    close(fd);
  }
  return img;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  const char *path = argv[1];
  const char *cmd  = argv[2];

  FILE *img = !strcmp(cmd, "restore") ? open_restore_image(path) : fopen(path, "rb");
  if (!img) {
    if (strcmp(cmd, "restore"))
      perror("open image");
    return 2;
  }

  bsfs_super_block sb;
  if (load_super(img, &sb) != 0 || validate_backing_size(img, &sb) != 0) {
    fclose(img);
    return 2;
  }

  int rc = 0;
  if (!strcmp(cmd, "list") && (argc == 3 || argc == 4)) {
    rc = cmd_list(img, &sb, argc >= 4 ? argv[3] : NULL);
  } else if (!strcmp(cmd, "show") && argc == 4) {
    uint32_t id;
    if (parse_id(argv[3], &id) != 0) {
      fprintf(stderr, "invalid recovery id '%s'\n", argv[3]);
      rc = 2;
    } else {
      rc = cmd_show(img, &sb, id);
    }
  } else if (!strcmp(cmd, "stats") && argc == 3) {
    rc = cmd_stats(img, &sb);
  } else if (!strcmp(cmd, "restore") && (argc == 4 || argc == 5)) {
    uint32_t id;
    if (parse_id(argv[3], &id) != 0) {
      fprintf(stderr, "invalid recovery id '%s'\n", argv[3]);
      rc = 2;
    } else {
      rc = cmd_restore(img, &sb, id, argc == 5 ? argv[4] : NULL);
    }
  } else {
    usage(argv[0]);
    rc = 2;
  }

  fclose(img);
  return rc;
}
