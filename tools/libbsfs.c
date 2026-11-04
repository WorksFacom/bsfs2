/**
 * @file libbsfs.c
 * @brief libbsfs, see libbsfs.h.
 *
 * the tree code follows the kernel's btree.c rules (separators copy the right leaf's first entry, a key equal
 * to one goes right) but allocates first-fit from the txn's bitmap and writes nodes with pwrite().
 */
#include "libbsfs.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BLKGETSIZE64
/// for libc headers that lack it, same value as <linux/fs.h>
#define BLKGETSIZE64 _IOR(0x12, 114, uint64_t)
#endif

int vec_contains(const struct u32_vec *v, uint32_t value) {
  for (size_t i = 0; i < v->count; i++)
    if (v->items[i] == value)
      return 1;
  return 0;
}

int vec_push_unique(struct u32_vec *v, uint32_t value) {
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

void vec_remove(struct u32_vec *v, uint32_t value) {
  for (size_t i = 0; i < v->count; i++) {
    if (v->items[i] == value) {
      v->items[i] = v->items[v->count - 1];
      v->count--;
      return;
    }
  }
}

int bsfs_read_block(int fd, uint32_t block, void *buf) {
  off_t  off  = (off_t)block * BSFS_BLOCK_SIZE;
  size_t done = 0;

  while (done < BSFS_BLOCK_SIZE) {
    ssize_t n = pread(fd, (uint8_t *)buf + done, BSFS_BLOCK_SIZE - done, off + (off_t)done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    done += (size_t)n;
  }
  return 0;
}

int bsfs_write_block(int fd, uint32_t block, const void *buf) {
  off_t  off  = (off_t)block * BSFS_BLOCK_SIZE;
  size_t done = 0;

  while (done < BSFS_BLOCK_SIZE) {
    ssize_t n = pwrite(fd, (const uint8_t *)buf + done, BSFS_BLOCK_SIZE - done, off + (off_t)done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    done += (size_t)n;
  }
  return 0;
}

int bsfs_dev_sync(int fd) {
  return fsync(fd);
}

int bsfs_dev_size(int fd, uint64_t *bytes) {
  struct stat st;

  if (fstat(fd, &st) != 0)
    return -1;
  if (S_ISBLK(st.st_mode))
    return ioctl(fd, BLKGETSIZE64, bytes) == 0 ? 0 : -1;
  if (S_ISREG(st.st_mode) && st.st_size >= 0) {
    *bytes = (uint64_t)st.st_size;
    return 0;
  }
  return -1;
}

/*
 * O_EXCL catches a mounted block device but not an image mounted through loop, the loop driver just
 * holds the file open. so compare the backing file of every mounted loop device with the target.
 * attached but unmounted is fine. *checked = 0 means the mount table couldn't be read.
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

int bsfs_dev_open(const char *path, int writable) {
  struct stat st;
  int         fd, flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;

  if (stat(path, &st) != 0) {
    perror(path);
    return -1;
  }
  if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
    fprintf(stderr, "%s: not a regular image or block device\n", path);
    return -1;
  }
  if (writable && S_ISBLK(st.st_mode)) {
    flags |= O_EXCL;
  } else if (writable) {
    int checked;

    if (image_is_mounted_via_loop(path, &checked)) {
      fprintf(stderr, "%s is mounted through a loop device, unmount it first\n", path);
      return -1;
    }
    if (!checked)
      fprintf(stderr, "warning: cannot read /proc/self/mountinfo, unable to confirm %s is not mounted\n", path);
  }
  fd = open(path, flags);
  if (fd < 0) {
    fprintf(stderr, "%s: %s: %s\n", path, writable ? "open for exclusive access" : "open", strerror(errno));
    return -1;
  }
  if (writable && flock(fd, LOCK_EX | LOCK_NB) != 0) {
    perror("lock image");
    close(fd);
    return -1;
  }
  return fd;
}

int bsfs_load_super(int fd, bsfs_super_block *sb) {
  uint8_t     raw[BSFS_BLOCK_SIZE];
  const char *bad;
  uint64_t    bytes;

  if (bsfs_read_block(fd, 1, raw) != 0) {
    fprintf(stderr, "failed to read superblock\n");
    return -1;
  }
  memcpy(sb, raw, sizeof(*sb));
  bad = bsfs_super_check(sb);
  if (bad) {
    fprintf(stderr, "invalid BSFS superblock: %s\n", bad);
    return -1;
  }
  if (bsfs_dev_size(fd, &bytes) != 0) {
    perror("device size");
    return -1;
  }
  if (bytes < sb->fs_size) {
    fprintf(stderr, "target is smaller than the filesystem declared by its superblock\n");
    return -1;
  }
  return 0;
}

int bitmap_create(const bsfs_super_block *sb, struct bitmap_state *bm) {
  memset(bm, 0, sizeof(*bm));
  bm->blocks      = calloc(sb->block_bitmap_total, sizeof(*bm->blocks));
  bm->disk_blocks = calloc(sb->block_bitmap_total, sizeof(*bm->disk_blocks));
  if (!bm->blocks || !bm->disk_blocks) {
    bitmap_free(bm);
    return -1;
  }
  bm->count = sb->block_bitmap_total;
  for (uint32_t i = 0; i < bm->count; i++) {
    bm->disk_blocks[i]       = sb->block_bitmap_start + i;
    bm->blocks[i].next_block = i + 1 < bm->count ? sb->block_bitmap_start + i + 1 : INVALID_BLOCK;
  }
  return 0;
}

int bitmap_load(int fd, const bsfs_super_block *sb, struct bitmap_state *bm) {
  uint8_t *seen;
  uint32_t block;
  int      rc = -1;

  memset(bm, 0, sizeof(*bm));
  seen            = calloc(sb->block_bitmap_total, 1);
  bm->blocks      = calloc(sb->block_bitmap_total, sizeof(*bm->blocks));
  bm->disk_blocks = calloc(sb->block_bitmap_total, sizeof(*bm->disk_blocks));
  if (!seen || !bm->blocks || !bm->disk_blocks)
    goto out;
  bm->count = sb->block_bitmap_total;
  block     = sb->block_bitmap_start;

  for (uint32_t i = 0; i < bm->count; i++) {
    // same rule as the kernel's load_bitmap(), plus no block twice
    if (block < sb->block_bitmap_start || block - sb->block_bitmap_start >= bm->count ||
        seen[block - sb->block_bitmap_start]++) {
      fprintf(stderr, "invalid bitmap chain at block %u\n", block);
      goto out;
    }
    bm->disk_blocks[i] = block;
    if (bsfs_read_block(fd, block, &bm->blocks[i]) != 0) {
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
  free(seen);
  if (rc)
    bitmap_free(bm);
  return rc;
}

int bitmap_write(int fd, const struct bitmap_state *bm) {
  for (uint32_t i = 0; i < bm->count; i++)
    if (bsfs_write_block(fd, bm->disk_blocks[i], &bm->blocks[i]) != 0)
      return -1;
  return 0;
}

static int bitmap_index(const struct bitmap_state *bm, uint32_t block, uint32_t *map, uint32_t *bit) {
  uint32_t m = block / BITS_PER_BLOCK;

  if (m >= bm->count)
    return -1;
  *map = m;
  *bit = block % BITS_PER_BLOCK;
  return 0;
}

int bitmap_is_set(const struct bitmap_state *bm, uint32_t block) {
  uint32_t map, bit;

  if (bitmap_index(bm, block, &map, &bit) != 0)
    return 0;
  return !!(bm->blocks[map].bits[bit >> 3] & (uint8_t)(1U << (bit & 7)));
}

int bitmap_set(struct bitmap_state *bm, uint32_t block, int used) {
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

void bitmap_free(struct bitmap_state *bm) {
  free(bm->blocks);
  free(bm->disk_blocks);
  memset(bm, 0, sizeof(*bm));
}

static struct tree_desc make_tree(const char *name, size_t entry_size, entry_key_fn key, entry_cmp_fn cmp) {
  struct tree_desc d = {
      .name       = name,
      .entry_size = entry_size,
      .min_degree = bsfs_min_degree(entry_size),
      .key        = key,
      .cmp        = cmp,
  };
  return d;
}

struct tree_desc bsfs_inode_tree(void) {
  return make_tree("inode", sizeof(disk_mapped_inode_t), get_inode_key, compare_inode);
}

struct tree_desc bsfs_dir_tree(void) {
  return make_tree("directory", sizeof(bsfs_dentry), get_dir_entry_key, compare_dir_entry);
}

struct tree_desc bsfs_rec_tree(void) {
  return make_tree("recovery", sizeof(rec_entry_t), get_rec_entry_key, compare_rec_entry);
}

struct tree_desc bsfs_bspan_tree(void) {
  return make_tree("extent", sizeof(bspan_t), get_bspan_key, compare_bspan);
}

static uint32_t txn_alloc_block(struct bsfs_txn *tx) {
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

static void txn_release_new(struct bsfs_txn *tx, uint32_t block) {
  if (!vec_contains(&tx->allocated, block))
    return;
  vec_remove(&tx->allocated, block);
  (void)bitmap_set(tx->bitmap, block, 0);
}

static int txn_retire(struct bsfs_txn *tx, uint32_t block) {
  // a corrupt root pointing at metadata would free the superblock or a bitmap block at commit
  if (block < tx->sb->data_block_start || block >= tx->sb->total_blocks) {
    fprintf(stderr, "refusing to release block %u from outside the data region\n", block);
    return -1;
  }
  // never published, free it now instead of at commit
  if (vec_contains(&tx->allocated, block)) {
    txn_release_new(tx, block);
    return 0;
  }
  return vec_push_unique(&tx->retired, block);
}

void txn_destroy(struct bsfs_txn *tx) {
  free(tx->allocated.items);
  free(tx->retired.items);
  memset(tx, 0, sizeof(*tx));
}

int txn_commit(struct bsfs_txn *tx, const bsfs_super_block *new_sb) {
  uint8_t     super_raw[BSFS_BLOCK_SIZE];
  const char *bad = bsfs_super_check(new_sb);

  if (bad) {
    fprintf(stderr, "refusing to publish an invalid superblock: %s\n", bad);
    return -1;
  }
  if (bsfs_dev_sync(tx->fd) != 0) {
    perror("sync shadow metadata");
    return -1;
  }
  if (bitmap_write(tx->fd, tx->bitmap) != 0 || bsfs_dev_sync(tx->fd) != 0) {
    perror("persist allocated bitmap blocks");
    return -1;
  }
  if (bsfs_read_block(tx->fd, 1, super_raw) != 0) {
    fprintf(stderr, "failed to reread superblock before commit\n");
    return -1;
  }
  memcpy(super_raw, new_sb, sizeof(*new_sb));
  if (bsfs_write_block(tx->fd, 1, super_raw) != 0 || bsfs_dev_sync(tx->fd) != 0) {
    fprintf(stderr, "superblock publication may be incomplete; inspect the image before retrying\n");
    return -2;
  }

  if (!tx->retired.count)
    return 0;
  for (size_t i = 0; i < tx->retired.count; i++)
    (void)bitmap_set(tx->bitmap, tx->retired.items[i], 0);
  if (bitmap_write(tx->fd, tx->bitmap) != 0 || bsfs_dev_sync(tx->fd) != 0)
    fprintf(stderr, "warning: new roots are committed, but obsolete metadata blocks may remain allocated\n");
  return 0;
}

int tree_read(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t block, struct disk_node **out) {
  struct disk_node *node;
  uint32_t          max_entries = (uint32_t)(2 * d->min_degree - 1);

  *out = NULL;
  // nodes only live in the data region, else a corrupt root gets the superblock or a bitmap block cloned and freed
  if (block < tx->sb->data_block_start || block >= tx->sb->total_blocks || (tx->bitmap && !bitmap_is_set(tx->bitmap, block))) {
    fprintf(stderr, "%s tree references invalid/unallocated block %u\n", d->name, block);
    return -1;
  }
  node = malloc(sizeof(*node));
  if (!node)
    return -1;
  node->block = block;
  if (bsfs_read_block(tx->fd, block, &node->raw) != 0) {
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

static int tree_write(struct bsfs_txn *tx, const struct disk_node *node) {
  return bsfs_write_block(tx->fd, node->block, &node->raw);
}

uint8_t *tree_entries(struct disk_node *node) {
  return node->raw.data;
}

uint32_t *tree_children(struct disk_node *node, const struct tree_desc *d) {
  return bsfs_node_children(&node->raw, d->min_degree, d->entry_size);
}

static int tree_new_node(struct bsfs_txn *tx, uint8_t is_leaf, struct disk_node **out) {
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

/* cow, blocks allocated in this txn were never published so they're already private */
static int tree_make_private(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t old_block, uint32_t *out_block) {
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

/* first entry not below @entry, the slot it would occupy in a leaf */
static uint32_t lower_bound(struct disk_node *node, const struct tree_desc *d, const void *entry) {
  uint32_t i = 0;

  while (i < node->raw.num_entries && d->cmp(tree_entries(node) + i * d->entry_size, entry) < 0)
    i++;
  return i;
}

static int entry_matches(struct disk_node *node, const struct tree_desc *d, uint32_t i, const void *entry) {
  return i < node->raw.num_entries && d->cmp(tree_entries(node) + i * d->entry_size, entry) == 0;
}

/* b+ tree, a key equal to a separator lives in the right child */
static uint32_t descend(struct disk_node *node, const struct tree_desc *d, const void *entry) {
  uint32_t i = lower_bound(node, d, entry);

  return entry_matches(node, d, i, entry) ? i + 1 : i;
}

int tree_search(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t root, const void *needle, void *out) {
  uint32_t block = root;
  unsigned depth = 0;

  while (block) {
    struct disk_node *node;

    if (depth++ > MAX_TREE_DEPTH) {
      fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
      return -1;
    }
    if (tree_read(tx, d, block, &node) != 0)
      return -1;
    if (node->raw.is_leaf) {
      uint32_t i     = lower_bound(node, d, needle);
      int      found = entry_matches(node, d, i, needle);

      if (found && out)
        memcpy(out, tree_entries(node) + i * d->entry_size, d->entry_size);
      free(node);
      return found;
    }
    block = tree_children(node, d)[descend(node, d, needle)];
    free(node);
  }
  return 0;
}

static int tree_split_child(struct bsfs_txn *tx, const struct tree_desc *d, struct disk_node *parent, uint32_t index) {
  struct disk_node *left = NULL, *right = NULL;
  uint32_t         *children       = tree_children(parent, d);
  uint8_t          *parent_entries = tree_entries(parent);
  uint32_t          left_block;
  size_t            size = d->entry_size;

  // tree_read only bounds num_entries, so a corrupt node may have no room for the separator and the
  // shifts below would write past the entry area
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
  memcpy(tree_entries(right), tree_entries(left) + (size_t)d->min_degree * size, (size_t)(d->min_degree - 1) * size);
  if (!left->raw.is_leaf)
    memcpy(tree_children(right, d), tree_children(left, d) + d->min_degree, (size_t)d->min_degree * sizeof(uint32_t));

  // leaves keep the middle entry and copy it up, internal nodes move it up
  left->raw.num_entries = left->raw.is_leaf ? d->min_degree : d->min_degree - 1;
  memmove(&children[index + 2], &children[index + 1], (parent->raw.num_entries - index) * sizeof(uint32_t));
  children[index + 1] = right->block;
  memmove(parent_entries + (index + 1) * size, parent_entries + index * size, (parent->raw.num_entries - index) * size);
  if (left->raw.is_leaf)
    memcpy(parent_entries + index * size, tree_entries(right), size);
  else
    memcpy(parent_entries + index * size, tree_entries(left) + (size_t)(d->min_degree - 1) * size, size);
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

static int tree_insert_nonfull(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t block, const void *entry,
                               unsigned depth) {
  struct disk_node *node = NULL, *child = NULL;
  uint8_t          *entries;
  uint32_t          i;
  size_t            size = d->entry_size;
  int               rc   = -1;

  if (depth > MAX_TREE_DEPTH) {
    fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  entries = tree_entries(node);
  if (node->raw.is_leaf) {
    // the pre-emptive split should leave room, but a corrupt leaf can already be full
    if (node->raw.num_entries >= (uint32_t)(2 * d->min_degree - 1)) {
      fprintf(stderr, "corrupt %s node block %u (insert into a full leaf)\n", d->name, block);
      goto out;
    }
    i = node->raw.num_entries;
    while (i > 0 && d->cmp(entries + (i - 1) * size, entry) > 0)
      i--;
    memmove(entries + (i + 1) * size, entries + i * size, (node->raw.num_entries - i) * size);
    memcpy(entries + i * size, entry, size);
    node->raw.num_entries++;
    rc = tree_write(tx, node);
    goto out;
  }

  i = descend(node, d, entry);
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
  rc    = tree_insert_nonfull(tx, d, tree_children(node, d)[i], entry, depth + 1);

out:
  free(child);
  free(node);
  return rc;
}

int tree_insert(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry) {
  struct disk_node *node = NULL, *new_root = NULL;
  uint32_t          private_root;

  if (tree_search(tx, d, *root, entry, NULL) != 0)
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

static int tree_update_rec(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t block, const void *entry, unsigned depth) {
  struct disk_node *node = NULL;
  int               rc   = -1;

  if (depth > MAX_TREE_DEPTH) {
    fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  if (node->raw.is_leaf) {
    uint32_t i = lower_bound(node, d, entry);

    if (!entry_matches(node, d, i, entry))
      goto out;
    memcpy(tree_entries(node) + i * d->entry_size, entry, d->entry_size);
    rc = tree_write(tx, node);
  } else {
    uint32_t idx = descend(node, d, entry), child;

    if (tree_make_private(tx, d, tree_children(node, d)[idx], &child) != 0)
      goto out;
    tree_children(node, d)[idx] = child;
    if (tree_write(tx, node) != 0)
      goto out;
    rc = tree_update_rec(tx, d, child, entry, depth + 1);
  }

out:
  free(node);
  return rc;
}

int tree_update(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t *root, const void *entry) {
  uint32_t private_root;

  if (*root == 0 || tree_make_private(tx, d, *root, &private_root) != 0)
    return -1;
  *root = private_root;
  return tree_update_rec(tx, d, *root, entry, 0);
}

struct walk_state {
  tree_visit_fn  visit;
  void          *ctx;
  uint8_t       *prev;
  int            have_prev;
  int            stopped;
  struct u32_vec nodes;
};

static int walk_node(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t block, struct walk_state *w, unsigned depth) {
  struct disk_node *node = NULL;
  int               rc   = -1;

  if (depth > MAX_TREE_DEPTH || vec_contains(&w->nodes, block) || vec_push_unique(&w->nodes, block) != 0) {
    fprintf(stderr, "%s tree contains a cycle or is too deep\n", d->name);
    return -1;
  }
  if (tree_read(tx, d, block, &node) != 0)
    return -1;
  if (node->raw.is_leaf) {
    for (uint32_t i = 0; i < node->raw.num_entries && !w->stopped; i++) {
      const uint8_t *entry = tree_entries(node) + i * d->entry_size;

      if (w->have_prev && d->cmp(w->prev, entry) >= 0) {
        fprintf(stderr, "%s tree leaf records are not strictly ordered\n", d->name);
        goto out;
      }
      memcpy(w->prev, entry, d->entry_size);
      w->have_prev = 1;
      if (w->visit) {
        int r = w->visit(entry, w->ctx);
        if (r < 0)
          goto out;
        w->stopped = r > 0;
      }
    }
  } else {
    for (uint32_t i = 0; i <= node->raw.num_entries && !w->stopped; i++)
      if (walk_node(tx, d, tree_children(node, d)[i], w, depth + 1) != 0)
        goto out;
  }
  rc = 0;

out:
  free(node);
  return rc;
}

int tree_walk(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t root, tree_visit_fn visit, void *ctx,
              struct u32_vec *nodes) {
  struct walk_state w = {.visit = visit, .ctx = ctx};
  int               rc;

  if (!root)
    return 0;
  w.prev = malloc(d->entry_size);
  if (!w.prev)
    return -1;
  rc = walk_node(tx, d, root, &w, 0);
  free(w.prev);
  if (nodes)
    *nodes = w.nodes;
  else
    free(w.nodes.items);
  return rc;
}

struct entry_collection {
  const struct tree_desc *d;
  uint8_t                *entries;
  size_t                  count;
  size_t                  cap;
};

static int collection_add(const void *entry, void *vctx) {
  struct entry_collection *c = vctx;

  if (c->count == c->cap) {
    size_t   new_cap = c->cap ? c->cap * 2 : 32;
    uint8_t *grown;

    if (new_cap < c->cap || new_cap > SIZE_MAX / c->d->entry_size)
      return -1;
    grown = realloc(c->entries, new_cap * c->d->entry_size);
    if (!grown)
      return -1;
    c->entries = grown;
    c->cap     = new_cap;
  }
  memcpy(c->entries + c->count * c->d->entry_size, entry, c->d->entry_size);
  c->count++;
  return 0;
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

static int tree_bulk_build(struct bsfs_txn *tx, const struct tree_desc *d, const uint8_t *entries, size_t count,
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
    size_t            groups    = level_count - i;
    // ceil(remaining / groups) spreads entries evenly so the last node isn't underfull
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
    struct level_item *parents      = calloc(parent_count, sizeof(*parents));
    size_t             child_offset = 0;

    if (!parents)
      goto err;
    for (size_t p = 0; p < parent_count; p++) {
      struct disk_node *parent;
      size_t            remaining = level_count - child_offset;
      size_t            groups    = parent_count - p;
      size_t            children  = remaining / groups + !!(remaining % groups);

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

int tree_rebuild_without_key(struct bsfs_txn *tx, const struct tree_desc *d, uint32_t old_root, uint32_t remove_key,
                             uint32_t *new_root) {
  struct entry_collection c     = {.d = d};
  struct u32_vec          nodes = {0};
  uint8_t                *filtered = NULL;
  size_t                  kept = 0, removed = 0;
  int                     rc = -1;

  if (!old_root || tree_walk(tx, d, old_root, collection_add, &c, &nodes) != 0)
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
    fprintf(stderr, "%s key %u is missing or duplicated\n", d->name, remove_key);
    goto out;
  }
  if (tree_bulk_build(tx, d, filtered, kept, new_root) != 0)
    goto out;
  for (size_t i = 0; i < nodes.count; i++)
    if (txn_retire(tx, nodes.items[i]) != 0)
      goto out;
  rc = 0;

out:
  free(filtered);
  free(c.entries);
  free(nodes.items);
  return rc;
}
