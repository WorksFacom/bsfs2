/**
 * @file reclaim_stats.c
 * @brief the debugfs file behind reclaim_stats.h.
 */
#include "../include/reclaim_stats.h"

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

static struct dentry *bsfs_debugfs_root;

static void reclaim_stats_release(struct kref *reference) {
  struct bsfs_reclaim_stats *stats = container_of(reference, struct bsfs_reclaim_stats, references);

  kfree(stats);
}

static int reclaim_stats_show(struct seq_file *stream, void *unused) {
  struct bsfs_reclaim_stats *stats = stream->private;
  struct bsfs_reclaim_error  errors[BSFS_RECLAIM_ERROR_SLOTS];
  unsigned long              flags;

  (void)unused;

  seq_printf(stream, "attempts %lld\n", atomic64_read(&stats->attempts));
  seq_printf(stream, "successes %lld\n", atomic64_read(&stats->successes));
  seq_printf(stream, "entries_purged %lld\n", atomic64_read(&stats->entries_purged));
  seq_printf(stream, "blocks_reclaimed %lld\n", atomic64_read(&stats->blocks_reclaimed));
  seq_printf(stream, "pre_commit_ns %lld\n", atomic64_read(&stats->pre_commit_ns));
  seq_printf(stream, "purge_ns %lld\n", atomic64_read(&stats->purge_ns));
  seq_printf(stream, "post_commit_ns %lld\n", atomic64_read(&stats->post_commit_ns));
  seq_printf(stream, "failures %lld\n", atomic64_read(&stats->failures));

  spin_lock_irqsave(&stats->error_lock, flags);
  memcpy(errors, stats->errors, sizeof(errors));
  spin_unlock_irqrestore(&stats->error_lock, flags);

  for (unsigned int i = 0; i < BSFS_RECLAIM_ERROR_SLOTS; i++) {
    if (errors[i].count)
      seq_printf(stream, "error_code_%d %llu\n", errors[i].error, errors[i].count);
  }

  return 0;
}

static int reclaim_stats_open(struct inode *inode, struct file *file) {
  struct bsfs_reclaim_stats *stats = inode->i_private;
  int                        error;

  if (!kref_get_unless_zero(&stats->references))
    return -ENODEV;
  error = single_open(file, reclaim_stats_show, stats);
  if (error)
    kref_put(&stats->references, reclaim_stats_release);
  return error;
}

static int reclaim_stats_file_release(struct inode *inode, struct file *file) {
  struct seq_file           *stream = file->private_data;
  struct bsfs_reclaim_stats *stats  = stream->private;
  int                        error;

  error = single_release(inode, file);
  kref_put(&stats->references, reclaim_stats_release);
  return error;
}

static const struct file_operations reclaim_stats_fops = {
    .owner   = THIS_MODULE,
    .open    = reclaim_stats_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = reclaim_stats_file_release,
};

int bsfs_reclaim_debugfs_init(void) {
  bsfs_debugfs_root = debugfs_create_dir("bsfs", NULL);
  if (IS_ERR(bsfs_debugfs_root)) {
    int error = PTR_ERR(bsfs_debugfs_root);

    bsfs_debugfs_root = NULL;
    if (error == -ENODEV)
      return 0;
    return error;
  }
  return 0;
}

void bsfs_reclaim_debugfs_exit(void) {
  debugfs_remove_recursive(bsfs_debugfs_root);
  bsfs_debugfs_root = NULL;
}

void bsfs_reclaim_stats_mount(struct super_block *sb) {
  struct bsfs_super_info    *sbi   = bsfs_sb(sb);
  struct bsfs_reclaim_stats *stats;
  struct dentry             *file;

  stats = kzalloc(sizeof(*stats), GFP_KERNEL);
  if (!stats)
    return;
  kref_init(&stats->references);
  sbi->reclaim_stats = stats;

  atomic64_set(&stats->attempts, 0);
  atomic64_set(&stats->successes, 0);
  atomic64_set(&stats->entries_purged, 0);
  atomic64_set(&stats->blocks_reclaimed, 0);
  atomic64_set(&stats->pre_commit_ns, 0);
  atomic64_set(&stats->purge_ns, 0);
  atomic64_set(&stats->post_commit_ns, 0);
  atomic64_set(&stats->failures, 0);
  spin_lock_init(&stats->error_lock);
  memset(stats->errors, 0, sizeof(stats->errors));
  stats->debugfs_dir = NULL;

  if (!bsfs_debugfs_root)
    return;

  stats->debugfs_dir = debugfs_create_dir(sb->s_id, bsfs_debugfs_root);
  if (IS_ERR(stats->debugfs_dir)) {
    stats->debugfs_dir = NULL;
    return;
  }
  file = debugfs_create_file("reclaim_stats", 0444, stats->debugfs_dir, stats, &reclaim_stats_fops);
  if (IS_ERR(file)) {
    debugfs_remove_recursive(stats->debugfs_dir);
    stats->debugfs_dir = NULL;
  }
}

void bsfs_reclaim_stats_unmount(struct super_block *sb) {
  struct bsfs_super_info    *sbi   = bsfs_sb(sb);
  struct bsfs_reclaim_stats *stats = sbi->reclaim_stats;

  if (!stats)
    return;

  debugfs_remove_recursive(stats->debugfs_dir);
  stats->debugfs_dir = NULL;
  sbi->reclaim_stats = NULL;
  kref_put(&stats->references, reclaim_stats_release);
}

void bsfs_reclaim_stats_failure(struct super_block *sb, int error) {
  struct bsfs_reclaim_stats *stats = bsfs_sb(sb)->reclaim_stats;
  unsigned long              flags;
  int                        empty = -1;

  if (!stats)
    return;

  atomic64_inc(&stats->failures);
  spin_lock_irqsave(&stats->error_lock, flags);
  for (unsigned int i = 0; i < BSFS_RECLAIM_ERROR_SLOTS; i++) {
    if (stats->errors[i].count && stats->errors[i].error == error) {
      stats->errors[i].count++;
      spin_unlock_irqrestore(&stats->error_lock, flags);
      return;
    }
    if (!stats->errors[i].count && empty < 0)
      empty = (int)i;
  }
  if (empty >= 0) {
    stats->errors[empty].error = error;
    stats->errors[empty].count = 1;
  }
  spin_unlock_irqrestore(&stats->error_lock, flags);
}
