/*
 * recovery.bsfs - userspace tool to inspect the BSFS recovery tree.
 *
 * opens a bsfs image as a plain file (read-only) and parses the on-disk recovery
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
 *
 * restore/purge aren't implemented yet. they'd mutate the inode/dentry/recovery
 * trees and the block bitmap, which needs a userspace B-tree write and allocator
 * layer this repo doesn't have.
 */
#include "../include/bsfs_shared.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ----- low level block IO ------------------------------------------------ */

static int read_block(FILE *img, uint32_t block, void *buf) {
  if (fseek(img, (long)block * BSFS_BLOCK_SIZE, SEEK_SET) != 0)
    return -1;
  if (fread(buf, BSFS_BLOCK_SIZE, 1, img) != 1)
    return -1;
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
  localtime_r(&t, &tm);
  strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

/* file_type uses the kernel's live convention (src/inode.c): 0=DIR, 1=FILE. */
static const char *type_str(uint8_t file_type) {
  return file_type == 0 ? "DIR" : "FILE";
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
static int walk_rec(FILE *img, const bsfs_super_block *sb, uint32_t block, int t, rec_visit_fn visit, void *ctx) {
  if (block == 0)
    return 0;
  if (block >= sb->total_blocks) {
    fprintf(stderr, "recovery node block %u out of range (total_blocks=%u)\n", block, sb->total_blocks);
    return -1;
  }

  uint8_t       raw[BSFS_BLOCK_SIZE];
  btree_node_t *n = (btree_node_t *)raw;
  if (read_block(img, block, raw) != 0) {
    fprintf(stderr, "failed to read recovery node block %u\n", block);
    return -1;
  }

  uint32_t maxn = (uint32_t)(2 * t - 1);
  if (n->num_entries > maxn) {
    fprintf(stderr, "recovery node block %u: num_entries=%u exceeds max %u\n", block, n->num_entries, maxn);
    return -1;
  }

  rec_entry_t *ents = (rec_entry_t *)node_entries(n);
  uint32_t    *kids = node_children(n, t, sizeof(rec_entry_t));

  for (uint32_t i = 0; i < n->num_entries; i++) {
    if (!n->is_leaf) {
      if (walk_rec(img, sb, kids[i], t, visit, ctx) != 0)
        return -1;
    }
    if (visit(&ents[i], ctx))
      return 0; /* visitor asked to stop */
  }
  if (!n->is_leaf)
    return walk_rec(img, sb, kids[n->num_entries], t, visit, ctx);
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
  return 0;
}

/* --- list --- */

struct list_ctx {
  const char *filter; /* exact original_name match, or NULL for all */
  size_t      printed;
};

static int visit_list(const rec_entry_t *e, void *vctx) {
  struct list_ctx *c = vctx;
  if (c->filter && strcmp(e->original_name, c->filter) != 0)
    return 0;
  char delbuf[32], expbuf[32];
  fmt_time(e->deleted_at, delbuf, sizeof(delbuf));
  fmt_time(e->retention_until, expbuf, sizeof(expbuf));
  printf("%-6u %-5s %-8u %-19s %-19s %-10u %s\n", e->recovery_id, type_str(e->file_type), e->file_size, delbuf, expbuf,
         e->original_parent_ino, e->original_name);
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
  if (walk_rec(img, sb, sb->recovery_root, t, visit_list, &c) != 0) {
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
    if (walk_rec(img, sb, sb->recovery_root, t, visit_show, &c) != 0) {
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
  printf("  original_name        %s\n", e->original_name);
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
    if (walk_rec(img, sb, sb->recovery_root, t, visit_stats, &c) != 0) {
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
          "  %s <img> stats\n",
          p, p, p);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  const char *path = argv[1];
  const char *cmd  = argv[2];

  FILE *img = fopen(path, "rb");
  if (!img) {
    perror("open image");
    return 2;
  }

  bsfs_super_block sb;
  if (load_super(img, &sb) != 0) {
    fclose(img);
    return 2;
  }

  int rc = 0;
  if (!strcmp(cmd, "list")) {
    rc = cmd_list(img, &sb, argc >= 4 ? argv[3] : NULL);
  } else if (!strcmp(cmd, "show") && argc >= 4) {
    rc = cmd_show(img, &sb, (uint32_t)strtoul(argv[3], NULL, 0));
  } else if (!strcmp(cmd, "stats")) {
    rc = cmd_stats(img, &sb);
  } else {
    usage(argv[0]);
    rc = 2;
  }

  fclose(img);
  return rc;
}
