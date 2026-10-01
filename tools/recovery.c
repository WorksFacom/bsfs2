/*
 * recovery.bsfs, offline inspect/restore for the bsfs recovery tree.
 *
 * reads an unmounted image or block device through libbsfs, which shares the on-disk format with the
 * kernel via include/bsfs_shared.h, no kernel build or qemu needed.
 * the kernel fills the tree with snapshots of deleted files at evict time.
 *
 *   recovery.bsfs <img> list [name]   list recovery entries (optional name filter)
 *   recovery.bsfs <img> show <id>     show all fields of one entry by recovery_id
 *   recovery.bsfs <img> stats         summary: count, reclaimable size, age range
 *   recovery.bsfs <img> restore <id> [absolute-destination-directory]
 *
 * restore cow's the metadata, syncs allocations before publishing the new roots in the superblock,
 * then frees what it replaced. the snapshot's span tree moves to the new inode as is, data blocks
 * are never copied or freed.
 */
#include "libbsfs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* "YYYY-mm-dd HH:MM:SS", "-" for zero */
static void fmt_time(uint64_t ts, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  if (ts == 0) {
    snprintf(buf, cap, "-");
    return;
  }
  time_t    t = (time_t)ts;
  struct tm tm;
  // a garbage timestamp makes localtime_r fail and leaves tm uninitialised
  if (!localtime_r(&t, &tm) || strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm) == 0)
    snprintf(buf, cap, "?");
}

static const char *type_str(uint8_t file_type) {
  return file_type == BSFS_REC_TYPE_DIR ? "DIR" : "FILE";
}

/* off disk, may not be NUL terminated */
static int name_len(const rec_entry_t *e) {
  return (int)strnlen(e->original_name, sizeof(e->original_name));
}

struct list_ctx {
  const char *filter; /* exact original_name match, or NULL for all */
  size_t      printed;
};

static int visit_list(const void *entry, void *vctx) {
  const rec_entry_t *e = entry;
  struct list_ctx   *c = vctx;
  int                len = name_len(e);

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

static int cmd_list(struct bsfs_txn *tx, const char *filter) {
  struct tree_desc rec_tree = bsfs_rec_tree();
  struct list_ctx  c        = {.filter = filter};

  if (tx->sb->recovery_root == 0) {
    printf("No recovery entries.\n");
    return 0;
  }

  printf("Recovery entries%s:\n", filter ? " (filtered)" : "");
  printf("%-6s %-5s %-8s %-19s %-19s %-10s %s\n", "ID", "Type", "Size", "Deleted At", "Retain Until", "ParentIno", "Name");
  printf("%-6s %-5s %-8s %-19s %-19s %-10s %s\n", "--", "----", "----", "-------------------", "-------------------", "----------", "----");

  if (tree_walk(tx, &rec_tree, tx->sb->recovery_root, visit_list, &c, NULL) != 0) {
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

static int cmd_show(struct bsfs_txn *tx, uint32_t id) {
  struct tree_desc rec_tree = bsfs_rec_tree();
  rec_entry_t      needle = {.recovery_id = id}, e;
  int              found  = tree_search(tx, &rec_tree, tx->sb->recovery_root, &needle, &e);

  if (found < 0) {
    fprintf(stderr, "error reading recovery entries\n");
    return 2;
  }
  if (!found) {
    fprintf(stderr, "recovery id %u not found\n", id);
    return 2;
  }

  char delbuf[32], expbuf[32];
  fmt_time(e.deleted_at, delbuf, sizeof(delbuf));
  fmt_time(e.retention_until, expbuf, sizeof(expbuf));

  printf("Recovery entry %u:\n", e.recovery_id);
  printf("  original_name        %.*s\n", name_len(&e), e.original_name);
  printf("  type                 %s\n", type_str(e.file_type));
  printf("  original_parent_ino  %u\n", e.original_parent_ino);
  printf("  inode_number         %u\n", e.inode_number);
  printf("  file_size            %u\n", e.file_size);
  printf("  block_count          %u\n", e.block_count);
  printf("  uid / gid            %u / %u\n", e.uid, e.gid);
  printf("  permissions          0%o\n", e.permissions);
  printf("  deleted_at           %s\n", delbuf);
  printf("  retention_until      %s\n", expbuf);
  printf("  btree_root           %u\n", e.btree_root);
  return 0;
}

struct stats_ctx {
  size_t   count;
  uint64_t total_size;
  uint64_t oldest;
  uint64_t newest;
};

static int visit_stats(const void *entry, void *vctx) {
  const rec_entry_t *e = entry;
  struct stats_ctx  *c = vctx;

  if (c->count == 0 || e->deleted_at < c->oldest)
    c->oldest = e->deleted_at;
  if (c->count == 0 || e->deleted_at > c->newest)
    c->newest = e->deleted_at;
  c->total_size += e->file_size;
  c->count++;
  return 0;
}

static int cmd_stats(struct bsfs_txn *tx) {
  struct tree_desc rec_tree = bsfs_rec_tree();
  struct stats_ctx c        = {0};

  if (tree_walk(tx, &rec_tree, tx->sb->recovery_root, visit_stats, &c, NULL) != 0) {
    fprintf(stderr, "error reading recovery entries\n");
    return 2;
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

struct extent_check {
  const bsfs_super_block    *sb;
  const struct bitmap_state *bitmap;
  uint64_t                   logical_end;
  uint32_t                   file_blocks;
  int                        have_span;
};

/* tree_walk already rejects cycles and unordered spans, this checks each span against the file and the bitmap */
static int visit_extent(const void *entry, void *vctx) {
  const bspan_t       *span        = entry;
  struct extent_check *check       = vctx;
  uint64_t             logical_end = (uint64_t)span->file_blk + span->length;
  uint64_t             disk_end    = (uint64_t)span->disk_blk + span->length;

  if (!span->length || logical_end > check->file_blocks || disk_end > check->sb->total_blocks ||
      span->disk_blk < check->sb->data_block_start || (check->have_span && span->file_blk < check->logical_end)) {
    fprintf(stderr, "invalid or overlapping extent at logical block %u\n", span->file_blk);
    return -1;
  }
  for (uint32_t j = 0; j < span->length; j++) {
    if (!bitmap_is_set(check->bitmap, span->disk_blk + j)) {
      fprintf(stderr, "extent references free data block %u\n", span->disk_blk + j);
      return -1;
    }
  }
  check->logical_end = logical_end;
  check->have_span   = 1;
  return 0;
}

static int validate_recovery_file(struct bsfs_txn *tx, const rec_entry_t *rec) {
  struct tree_desc    bspan_tree = bsfs_bspan_tree();
  struct extent_check check      = {
           .sb          = tx->sb,
           .bitmap      = tx->bitmap,
           .file_blocks = (rec->file_size + (uint64_t)BSFS_BLOCK_SIZE - 1) / BSFS_BLOCK_SIZE,
  };
  size_t len = strnlen(rec->original_name, sizeof(rec->original_name));

  if (rec->file_type != BSFS_REC_TYPE_FILE || !S_ISREG((mode_t)rec->permissions)) {
    fprintf(stderr, "recovery id %u is not a regular file\n", rec->recovery_id);
    return -1;
  }
  if (!len || len >= MAX_FILENAME || strchr(rec->original_name, '/') || !strcmp(rec->original_name, ".") ||
      !strcmp(rec->original_name, "..")) {
    fprintf(stderr, "recovery id %u has an invalid original name\n", rec->recovery_id);
    return -1;
  }
  /* a sparse file can have no spans at all, tree_walk takes root 0 as empty */
  return tree_walk(tx, &bspan_tree, rec->btree_root, visit_extent, &check, NULL);
}

/* 1 found, 0 not, -1 on error */
static int load_inode(struct bsfs_txn *tx, uint32_t inode_root, uint32_t ino, disk_mapped_inode_t *out) {
  struct tree_desc    inode_tree = bsfs_inode_tree();
  disk_mapped_inode_t needle     = {.ino = ino};

  return tree_search(tx, &inode_tree, inode_root, &needle, out);
}

static int inode_is_live_dir(const disk_mapped_inode_t *inode) {
  return inode->nlinks != 0 && S_ISDIR((mode_t)inode->mode);
}

static int resolve_destination_path(struct bsfs_txn *tx, uint32_t inode_root, const char *path, disk_mapped_inode_t *out) {
  struct tree_desc    dir_tree = bsfs_dir_tree();
  disk_mapped_inode_t current;
  char               *copy = NULL, *save = NULL, *component;
  int                 rc = -1;

  if (!path || path[0] != '/') {
    fprintf(stderr, "destination must be an absolute BSFS directory path\n");
    return -1;
  }
  if (load_inode(tx, inode_root, tx->sb->root_inode, &current) != 1 || !inode_is_live_dir(&current)) {
    fprintf(stderr, "BSFS root inode is missing or invalid\n");
    return -1;
  }
  copy = strdup(path);
  if (!copy)
    return -1;
  component = strtok_r(copy, "/", &save);
  while (component) {
    bsfs_dentry needle = {0}, found;
    size_t      len    = strlen(component);

    if (!len || len >= MAX_FILENAME || !strcmp(component, ".") || !strcmp(component, "..")) {
      fprintf(stderr, "destination contains an invalid path component\n");
      goto out;
    }
    memcpy(needle.filename, component, len + 1);
    if (tree_search(tx, &dir_tree, current.btree_root, &needle, &found) != 1 ||
        load_inode(tx, inode_root, found.ino, &current) != 1 || !inode_is_live_dir(&current)) {
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

static int resolve_destination(struct bsfs_txn *tx, uint32_t inode_root, const rec_entry_t *rec, const char *path,
                               disk_mapped_inode_t *out) {
  if (path)
    return resolve_destination_path(tx, inode_root, path, out);
  if (rec->original_parent_ino && load_inode(tx, inode_root, rec->original_parent_ino, out) == 1 && inode_is_live_dir(out))
    return 0;
  if (load_inode(tx, inode_root, tx->sb->root_inode, out) == 1 && inode_is_live_dir(out))
    return 0;
  fprintf(stderr, "neither the original parent nor the BSFS root is usable\n");
  return -1;
}

static int find_max_inode(struct bsfs_txn *tx, uint32_t root, uint32_t *out) {
  struct tree_desc inode_tree = bsfs_inode_tree();
  uint32_t         block      = root;
  unsigned         depth      = 0;

  *out = 0;
  while (block) {
    struct disk_node *node;
    if (depth++ > MAX_TREE_DEPTH) {
      fprintf(stderr, "%s tree contains a cycle or is too deep\n", inode_tree.name);
      return -1;
    }
    if (tree_read(tx, &inode_tree, block, &node) != 0)
      return -1;
    if (node->raw.is_leaf) {
      if (node->raw.num_entries)
        *out = get_inode_key(tree_entries(node) + (node->raw.num_entries - 1) * inode_tree.entry_size);
      free(node);
      return 0;
    }
    block = tree_children(node, &inode_tree)[node->raw.num_entries];
    free(node);
  }
  return 0;
}

static int cmd_restore(int fd, const bsfs_super_block *sb, uint32_t id, const char *destination) {
  struct bitmap_state bitmap     = {0};
  struct bsfs_txn     tx         = {.fd = fd, .sb = sb, .bitmap = &bitmap};
  struct tree_desc    inode_tree = bsfs_inode_tree();
  struct tree_desc    dir_tree   = bsfs_dir_tree();
  struct tree_desc    rec_tree   = bsfs_rec_tree();
  disk_mapped_inode_t parent, restored = {0}, existing;
  bsfs_dentry         dentry = {0}, collision;
  bsfs_super_block    new_sb = *sb;
  rec_entry_t         rec, rec_needle = {.recovery_id = id};
  uint32_t            inode_root = sb->inode_root, dir_root, recovery_root, ino, max_ino;
  uint32_t            now = (uint32_t)time(NULL);
  int                 found, rc = 2;

  if (bitmap_load(fd, sb, &bitmap) != 0)
    goto out;
  found = tree_search(&tx, &rec_tree, sb->recovery_root, &rec_needle, &rec);
  if (found != 1) {
    if (!found)
      fprintf(stderr, "recovery id %u not found\n", id);
    goto out;
  }
  if (validate_recovery_file(&tx, &rec) != 0 || resolve_destination(&tx, inode_root, &rec, destination, &parent) != 0)
    goto out;

  memcpy(dentry.filename, rec.original_name, strlen(rec.original_name) + 1);
  dentry.name_hash = hash_filename(dentry.filename);
  found            = tree_search(&tx, &dir_tree, parent.btree_root, &dentry, &collision);
  if (found != 0) {
    if (found > 0)
      fprintf(stderr, "destination already contains '%s'\n", rec.original_name);
    goto out;
  }

  // reuse the original inode number unless something took it
  ino   = rec.inode_number;
  found = ino ? load_inode(&tx, inode_root, ino, &existing) : 1;
  if (found < 0)
    goto out;
  if (!ino || found == 1) {
    if (find_max_inode(&tx, inode_root, &max_ino) != 0 || max_ino == UINT32_MAX) {
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
  if (tree_update(&tx, &inode_tree, &inode_root, &parent) != 0) {
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
  if (tree_rebuild_without_key(&tx, &rec_tree, sb->recovery_root, id, &recovery_root) != 0) {
    fprintf(stderr, "failed to remove recovery entry (metadata space exhausted?)\n");
    goto out;
  }

  new_sb.inode_root    = inode_root;
  new_sb.recovery_root = recovery_root;
  if (txn_commit(&tx, &new_sb) != 0)
    goto out;

  if (destination)
    printf("Restored recovery entry %u as %s/%s (inode %u).\n", id, !strcmp(destination, "/") ? "" : destination,
           rec.original_name, ino);
  else
    printf("Restored recovery entry %u as '%s' in parent inode %u (inode %u).\n", id, rec.original_name, parent.ino, ino);
  rc = 0;

out:
  txn_destroy(&tx);
  bitmap_free(&bitmap);
  return rc;
}

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

int main(int argc, char **argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  const char *path    = argv[1];
  const char *cmd     = argv[2];
  int         restore = !strcmp(cmd, "restore");

  int fd = bsfs_dev_open(path, restore);
  if (fd < 0)
    return 2;

  bsfs_super_block sb;
  if (bsfs_load_super(fd, &sb) != 0) {
    close(fd);
    return 2;
  }

  // read-only commands walk without the bitmap, only restore allocates
  struct bsfs_txn ro = {.fd = fd, .sb = &sb};
  uint32_t        id;
  int             rc;

  if (!strcmp(cmd, "list") && (argc == 3 || argc == 4)) {
    rc = cmd_list(&ro, argc >= 4 ? argv[3] : NULL);
  } else if (!strcmp(cmd, "stats") && argc == 3) {
    rc = cmd_stats(&ro);
  } else if ((!strcmp(cmd, "show") && argc == 4) || (restore && (argc == 4 || argc == 5))) {
    if (parse_id(argv[3], &id) != 0) {
      fprintf(stderr, "invalid recovery id '%s'\n", argv[3]);
      rc = 2;
    } else {
      rc = restore ? cmd_restore(fd, &sb, id, argc == 5 ? argv[4] : NULL) : cmd_show(&ro, id);
    }
  } else {
    usage(argv[0]);
    rc = 2;
  }

  close(fd);
  return rc;
}
