/**
 * @file rec_entry.c
 * @brief the recovery tree: adding, removing and purging snapshots.
 *
 * the kernel only records snapshots and purges them under space pressure. listing and restoring them is
 * recovery.bsfs's job, offline.
 */
#include "../include/rec_entry.h"
#include "../include/btree.h"
#include "../include/inode.h"

void bsfs_rec_tree_mount_init(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  mutex_init(&sbi->rec_lock);
  sbi->next_recovery_id     = 1;
  sbi->cached_recovery_root = INVALID_BLOCK;
}

static void rec_tree_sync_next_id(struct super_block *sb, btree_handle_t *rec_tree_h) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  if (rec_tree_h->root_block == 0) {
    sbi->next_recovery_id     = 1;
    sbi->cached_recovery_root = 0;
    return;
  }

  if (sbi->cached_recovery_root == rec_tree_h->root_block)
    return;

  struct btree_node *node;
  __u32              node_block = rec_tree_h->root_block;

  while (node_block && node_block != INVALID_BLOCK) {
    if (btree_read_node(sb, rec_tree_h, node_block, &node) != 0)
      break;

    if (node->node->is_leaf) {
      if (node->node->num_entries > 0) {
        rec_entry_t *last     = (rec_entry_t *)(bentries(node) + (node->node->num_entries - 1) * rec_tree_h->entry_size);
        sbi->next_recovery_id = last->recovery_id + 1;
      } else {
        sbi->next_recovery_id = 1;
      }
      btree_free_node(node);
      break;
    }

    __u32 child_block = bchildren(node, rec_tree_h)[node->node->num_entries];
    btree_free_node(node);
    node_block = child_block;
  }

  sbi->cached_recovery_root = rec_tree_h->root_block;
}

static __u32 get_next_rec_id(struct super_block *sb, btree_handle_t *rec_tree) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  rec_tree_sync_next_id(sb, rec_tree);
  return sbi->next_recovery_id++;
}

int bsfs_rec_tree_add(struct super_block *sb, btree_handle_t *rec_tree, rec_entry_t *entry) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  int                     err;

  lockdep_assert_held(&sbi->rec_lock);

  if (entry->recovery_id == 0)
    entry->recovery_id = get_next_rec_id(sb, rec_tree);

  if (entry->retention_until == 0 && entry->deleted_at != 0)
    entry->retention_until = entry->deleted_at + DEFAULT_RECOVERY_TTL;

  int attempts = 0;
  while (attempts < 8) {
    err = btree_insert(sb, rec_tree, entry);
    if (err == 0)
      break;
    entry->recovery_id = get_next_rec_id(sb, rec_tree);
    attempts++;
  }

  if (err != 0)
    return -EIO;

  if (entry->recovery_id >= sbi->next_recovery_id)
    sbi->next_recovery_id = entry->recovery_id + 1;

  if (rec_tree->root_block != 0 && sbi->super->recovery_root != rec_tree->root_block) {
    sbi->super->recovery_root = rec_tree->root_block;
    mark_buffer_dirty(sbi->bh);
    sbi->cached_recovery_root = rec_tree->root_block;
  }

  return 0;
}

/*
 * expired first, else oldest. internal entries are separator copies, so leaves only.
 */
static void rec_find_victim(struct super_block *sb, btree_handle_t *h, __u32 node_block, __u64 now, rec_entry_t *best, int *have) {
  struct btree_node *node;

  if (node_block == 0 || node_block == INVALID_BLOCK)
    return;

  if (btree_read_node(sb, h, node_block, &node) != 0)
    return;

  if (!node->node->is_leaf) {
    __u32 *children = bchildren(node, h);

    for (__u32 i = 0; i <= node->node->num_entries; i++)
      rec_find_victim(sb, h, children[i], now, best, have);
  } else {
    for (__u32 i = 0; i < node->node->num_entries; i++) {
      rec_entry_t *e = (rec_entry_t *)(bentries(node) + i * h->entry_size);

      int e_expired = (e->retention_until != 0 && e->retention_until <= now);
      int b_expired;

      if (!*have) {
        *best = *e;
        *have = 1;
        continue;
      }

      b_expired = (best->retention_until != 0 && best->retention_until <= now);

      if ((e_expired && !b_expired) || (e_expired == b_expired && e->deleted_at < best->deleted_at))
        *best = *e;
    }
  }

  btree_free_node(node);
}

/*
 * drops snapshots (expired, then oldest) and queues their blocks for release. deleting a file hands its span tree
 * to a snapshot, so this is the only way that space comes back. only runs under space pressure.
 * returns entries purged.
 */
unsigned int bsfs_rec_tree_purge(struct super_block *sb, unsigned int max_entries, __u64 *blocks_deferred) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  btree_handle_t         *rec_tree;
  rec_entry_t            *victim;
  unsigned int            purged = 0;

  if (sbi->super->recovery_root == 0)
    return 0;

  rec_tree = btree_create_rec_entry_handle();
  victim   = kmalloc(sizeof(*victim), GFP_NOFS);

  if (!rec_tree || !victim) {
    kfree(rec_tree);
    kfree(victim);
    return 0;
  }

  // last source of free space, may spend the whole reserve
  rec_tree->prio = BSFS_ALLOC_SHRINK;

  mutex_lock(&sbi->rec_lock);

  while (purged < max_entries) {
    int   have = 0;
    __u32 data_root;

    rec_tree->root_block = sbi->super->recovery_root;

    if (rec_tree->root_block == 0)
      break;

    rec_find_victim(sb, rec_tree, rec_tree->root_block, ktime_get_real_seconds(), victim, &have);

    if (!have)
      break;

    data_root = victim->btree_root;

    // unlink the snapshot before freeing its blocks, or it dangles
    if (!bsfs_rec_tree_remove(sb, rec_tree, victim->recovery_id)) {
      pr_warn("recovery purge could not remove id %u, giving up\n", victim->recovery_id);
      break;
    }

    if (data_root) {
      btree_handle_t *bspan_h = btree_create_bspan_handle();

      if (bspan_h) {
        bspan_h->prio       = BSFS_ALLOC_SHRINK;
        bspan_h->root_block = data_root;

        if (btree_deallocate_all(sb, bspan_h, data_root, blocks_deferred) != 0)
          pr_warn("recovery purge leaked the blocks of id %u\n", victim->recovery_id);

        kfree(bspan_h);
      }
    }

    purged++;
  }

  mutex_unlock(&sbi->rec_lock);

  kfree(rec_tree);
  kfree(victim);

  return purged;
}

int bsfs_rec_tree_remove(struct super_block *sb, btree_handle_t *rec_tree, __u32 recovery_id) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  lockdep_assert_held(&sbi->rec_lock);

  int ret = btree_remove(sb, rec_tree, recovery_id);

  if (ret && sbi->super->recovery_root != rec_tree->root_block) {
    sbi->super->recovery_root = rec_tree->root_block;
    mark_buffer_dirty(sbi->bh);
    sbi->cached_recovery_root = rec_tree->root_block;
  }

  return ret;
}
