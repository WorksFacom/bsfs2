#include "../include/rec_entry.h"
#include "../include/inode.h"

// TODO: rec tree CLI
// TODO: initialize rec_tree on mkfs - see old implementation
void bsfs_rec_tree_init(struct super_block *sb) {
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
    if (btree_read_node(sb, node_block, &node) != 0 || !node)
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
