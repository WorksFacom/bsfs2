/**
 * @file alloc.c
 * @brief block allocation, reserve accounting and deferred frees.
 */
#include "../include/alloc.h"
#include "../include/alloc_strategy.h"
#include "../include/rec_entry.h"
#include "../include/reclaim_stats.h"
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/ktime.h>
#include <linux/writeback.h>

/*
 * deferred frees, the cow write barrier.
 *
 * a new root is only durable once its owner is written (superblock for the inode and recovery trees,
 * the inode for per-inode trees). freeing replaced blocks right away lets them get reused while the
 * on-disk tree still points at them, so a crash leaves a tree full of someone else's blocks.
 * they wait here until a commit point, after everything dirty is on disk.
 */
// caller must hold sbi->pending_free_lock
static int __defer_free_block(struct bsfs_super_info *sbi, __u32 block) {
  if (sbi->pending_free_count == sbi->pending_free_cap) {
    size_t new_cap = (sbi->pending_free_cap == 0) ? 64 : (sbi->pending_free_cap * 2);
    __u32 *grown   = krealloc(sbi->pending_free, new_cap * sizeof(__u32), GFP_NOFS);

    if (!grown)
      return -ENOMEM;

    sbi->pending_free     = grown;
    sbi->pending_free_cap = new_cap;
  }

  sbi->pending_free[sbi->pending_free_count++] = block;
  return 0;
}

int bsfs_defer_free_block(struct super_block *sb, __u32 block) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  int                     ret;

  if (block == 0 || block == INVALID_BLOCK)
    return 0;

  mutex_lock(&sbi->pending_free_lock);
  ret = __defer_free_block(sbi, block);
  mutex_unlock(&sbi->pending_free_lock);

  if (ret) {
    // leaking until the next mkfs is ugly but safe, freeing now would break the barrier
    pr_warn_ratelimited("no memory to defer block %u, leaking it to keep the write barrier\n", block);
    return ret;
  }

  return 0;
}

int bsfs_defer_free_bspan(struct super_block *sb, const bspan_t *bspan) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  int                     ret = 0;

  mutex_lock(&sbi->pending_free_lock);

  for (__u32 i = 0; i < bspan->length; i++) {
    int err = __defer_free_block(sbi, bspan->disk_blk + i);

    if (err && !ret)
      ret = err;
  }

  mutex_unlock(&sbi->pending_free_lock);

  if (ret)
    pr_warn_ratelimited("no memory to defer span %u+%u, leaking it to keep the write barrier\n", bspan->disk_blk, bspan->length);

  return ret;
}

// puts a list back on the pending queue after a failed barrier
static void requeue_pending(struct super_block *sb, __u32 *blocks, size_t count) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  mutex_lock(&sbi->pending_free_lock);

  for (size_t i = 0; i < count; i++) {
    if (__defer_free_block(sbi, blocks[i]) != 0) {
      pr_warn("dropping deferred block %u after a failed barrier, it is leaked\n", blocks[i]);
      break;
    }
  }

  mutex_unlock(&sbi->pending_free_lock);
  kfree(blocks);
}

static int __commit_deferred_frees(struct super_block *sb, bool writeback) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                  *blocks;
  size_t                  count;
  int                     ret, err;

  // snapshot first. the writeback below only covers blocks already listed, anything deferred
  // meanwhile belongs to an unwritten root and waits for the next commit.
  mutex_lock(&sbi->pending_free_lock);
  count  = sbi->pending_free_count;
  blocks = sbi->pending_free;

  sbi->pending_free       = NULL;
  sbi->pending_free_count = 0;
  sbi->pending_free_cap   = 0;

  if (writeback)
    sbi->writeback_error = 0;

  mutex_unlock(&sbi->pending_free_lock);

  // per-inode roots only reach the inode tree via ->write_inode, so without this the barrier covers
  // the trees but not the pointers to them. ->sync_fs and ->put_super already did it.
  if (writeback)
    sync_inodes_sb(sb);

  // runs even with nothing to free, callers use it to publish roots. nodes and bitmap, then the
  // superblock, then the device cache.
  // node-before-superblock is best effort. both sit in the bdev mapping and sync_blockdev() picks the
  // order. making it strict means keeping sbi->bh clean until commit.
  ret = sync_blockdev(sb->s_bdev);

  if (sbi->bh) {
    err = sync_dirty_buffer(sbi->bh);

    if (!ret)
      ret = err;
  }

  err = blkdev_issue_flush(sb->s_bdev);

  if (!ret)
    ret = err;

  // a failed ->write_inode means the on-disk tree is still the old one, which points at exactly
  // these blocks. routine under ENOSPC since persisting an inode allocates.
  if (!ret)
    ret = sbi->writeback_error;

  if (ret) {
    if (ret == -ENOSPC)
      pr_warn_ratelimited("commit barrier out of space, keeping %zu deferred blocks\n", count);
    else
      pr_warn("commit barrier failed (%d), keeping %zu deferred blocks\n", ret, count);

    requeue_pending(sb, blocks, count);
    return ret;
  }

  for (size_t i = 0; i < count; i++) {
    if (deallocate_single_block(sb, blocks[i]) != 0)
      pr_warn("failed to release deferred block %u\n", blocks[i]);
  }

  kfree(blocks);
  return 0;
}

int bsfs_commit_deferred_frees(struct super_block *sb) {
  return __commit_deferred_frees(sb, false);
}

/*
 * true when the next mutation might not find a block.
 *
 * under cow a delete allocates before it releases, so with every free block parked on the deferred
 * list the fs can't delete its way out. entry points check this and commit while they still can,
 * a failed allocation mid-op leaves an error nobody can report usefully.
 */
static bool low_on_space(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  bool                    low;

  mutex_lock(&sbi->bitmap_lock);
  low = sbi->free_blocks <= sbi->reserved_blocks * 2;
  mutex_unlock(&sbi->bitmap_lock);

  return low;
}

bool bsfs_should_reclaim(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  bool                    pending;

  if (!low_on_space(sb))
    return false;

  mutex_lock(&sbi->pending_free_lock);
  pending = sbi->pending_free_count > 0;
  mutex_unlock(&sbi->pending_free_lock);

  // nothing parked, but recovery snapshots may be sitting on every deleted file's blocks
  return pending || sbi->super->recovery_root != 0;
}

/*
 * writes back dirty inodes, then commits. caller holds no fs locks, this waits on every dirty inode.
 * s_umount is a trylock on purpose. if it's held we're unmounting or freezing, and both commit anyway.
 */
int bsfs_reclaim_space(struct super_block *sb) {
  struct bsfs_reclaim_stats *stats = bsfs_sb(sb)->reclaim_stats;
  __u64                      blocks_deferred = 0;
  __u64                      started;
  unsigned int               purged = 0;
  int                        ret;

  if (stats)
    atomic64_inc(&stats->attempts);

  if (!down_read_trylock(&sb->s_umount)) {
    bsfs_reclaim_stats_failure(sb, -EBUSY);
    return -EBUSY;
  }

  started = ktime_get_mono_fast_ns();
  ret = __commit_deferred_frees(sb, true);
  if (stats)
    atomic64_add(ktime_get_mono_fast_ns() - started, &stats->pre_commit_ns);

  // still short, so the rest is held by recovery snapshots. this is the only place that space
  // comes back, and only because the alternative is failing the write.
  if (ret == 0 && low_on_space(sb)) {
    started = ktime_get_mono_fast_ns();
    purged = bsfs_rec_tree_purge(sb, BSFS_PURGE_BATCH, &blocks_deferred);
    if (stats)
      atomic64_add(ktime_get_mono_fast_ns() - started, &stats->purge_ns);
  }

  if (purged > 0) {
    if (stats)
      atomic64_add(purged, &stats->entries_purged);
    started = ktime_get_mono_fast_ns();
    ret = __commit_deferred_frees(sb, true);
    if (stats)
      atomic64_add(ktime_get_mono_fast_ns() - started, &stats->post_commit_ns);
    if (ret == 0) {
      if (stats) {
        atomic64_inc(&stats->successes);
        atomic64_add(blocks_deferred, &stats->blocks_reclaimed);
      }
    }
  }

  up_read(&sb->s_umount);

  if (ret)
    bsfs_reclaim_stats_failure(sb, ret);

  return ret;
}

/*
 * counts free blocks at mount and sizes the metadata reserve. btree_remove() clones a root-to-leaf
 * path before dropping anything, so a completely full fs could never be truncated or unlinked.
 */
void bsfs_init_block_counts(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   free_blocks = 0;
  __u32                   usable, reserve, i;

  mutex_lock(&sbi->bitmap_lock);

  for (i = sbi->super->data_block_start; i < sbi->super->total_blocks; i++) {
    if (bsfs_is_bit_free(sb, i))
      free_blocks++;
  }

  sbi->free_blocks = free_blocks;

  // a couple of full cow paths, capped so tiny images stay usable
  usable  = sbi->super->total_blocks - sbi->super->data_block_start;
  reserve = usable / 64;

  if (reserve > BSFS_META_RESERVE)
    reserve = BSFS_META_RESERVE;

  sbi->reserved_blocks = reserve;

  mutex_unlock(&sbi->bitmap_lock);
}

/*
 * the one place a block is chosen. the floor is caller policy (bsfs_alloc_prio_t) so it stays above
 * the strategy, which only picks which free block.
 *
 * if the strategy finds nothing while free_blocks disagrees, the index drifted. trust the strategy,
 * handing out a block it doesn't know about is how data gets corrupted.
 */
// caller must hold sbi->bitmap_lock
static __u32 __allocate_single_block(struct super_block *sb, __u32 floor) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   block;

  if (sbi->free_blocks <= floor)
    return INVALID_BLOCK;

  block = bsfs_alloc_strategy_alloc(sb);

  if (block == INVALID_BLOCK) {
    pr_warn_ratelimited("allocator found nothing with %u blocks free (index out of sync?)\n", sbi->free_blocks);
    return INVALID_BLOCK;
  }

  sbi->free_blocks--;
  BSFS_STAT_INC(sb, allocs);

  return block;
}

/*
 * file data. stops at the metadata reserve so truncate and unlink always have room.
 */
__u32 allocate_single_block(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   block;

  mutex_lock(&sbi->bitmap_lock);
  block = __allocate_single_block(sb, sbi->reserved_blocks);
  mutex_unlock(&sbi->bitmap_lock);

  if (block != INVALID_BLOCK)
    return block;

  // usually deferred frees holding blocks back, but the caller is mid-transaction under
  // truncate_mutex or inode_tree_lock and can't run writeback. the vfs entry point retries via
  // bsfs_reclaim_space().
  pr_warn_ratelimited("no blocks available (%zu deferred)\n", sbi->pending_free_count);

  return INVALID_BLOCK;
}

// caller must hold sbi->bitmap_lock
static __u32 prio_floor(struct bsfs_super_info *sbi, __u8 prio) {
  switch (prio) {
  case BSFS_ALLOC_SHRINK:
    return 0;
  case BSFS_ALLOC_WRITEBACK:
    return sbi->reserved_blocks / 2;
  default:
    return sbi->reserved_blocks;
  }
}

/*
 * tree metadata, every cow clone and split. may spend the reserve, shrinking a tree is the only
 * way a full fs recovers and shrinking allocates.
 */
__u32 allocate_meta_block(struct super_block *sb, __u8 prio) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   block;

  mutex_lock(&sbi->bitmap_lock);
  block = __allocate_single_block(sb, prio_floor(sbi, prio));
  mutex_unlock(&sbi->bitmap_lock);

  if (block == INVALID_BLOCK)
    pr_warn_ratelimited("no metadata blocks available at prio %u (%zu deferred)\n", prio, sbi->pending_free_count);

  return block;
}

int deallocate_single_block(struct super_block *sb, __u32 block_index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  if (block_index >= sbi->super->total_blocks) {
    printk(KERN_ERR "Invalid block index for deallocation: %u\n", block_index);
    return -1;
  }

  mutex_lock(&sbi->bitmap_lock);
  // rollback can free the same block twice, only a real transition counts
  if (bsfs_clear_bit(sb, block_index)) {
    sbi->free_blocks++;
    BSFS_STAT_INC(sb, frees);
    bsfs_alloc_strategy_freed(sb, block_index);
  }
  mutex_unlock(&sbi->bitmap_lock);
  return 0;
}

/*
 * logs the build-time strategy every mount, otherwise nothing tells you which .ko a test ran against.
 * unmount counter keys are stable and space separated, tests/alloc_guest.sh seds them out of dmesg.
 */
void bsfs_alloc_log_mount(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  pr_info("allocator '%s' on %s: %u blocks free, %u reserved\n", BSFS_ALLOC_STRATEGY_NAME, sb->s_id, sbi->free_blocks,
          sbi->reserved_blocks);
}

void bsfs_alloc_log_unmount(struct super_block *sb) {
#ifdef BSFS_ALLOC_STATS
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  char                    detail[96];

  mutex_lock(&sbi->bitmap_lock);
  bsfs_alloc_strategy_stats(sb, detail, sizeof(detail));

  pr_info("alloc[%s] allocs=%llu frees=%llu from_index=%llu from_cursor=%llu index_peak=%llu fallback_scans=%llu %s\n",
          BSFS_ALLOC_STRATEGY_NAME, sbi->alloc_stats.allocs, sbi->alloc_stats.frees, sbi->alloc_stats.from_index,
          sbi->alloc_stats.from_cursor, sbi->alloc_stats.index_peak, sbi->alloc_stats.fallback_scans, detail);
  mutex_unlock(&sbi->bitmap_lock);
#else
  (void)sb;
#endif
}
