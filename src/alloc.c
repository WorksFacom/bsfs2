/**
 * @file allocate_bsfs.c
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#include "../include/alloc.h"
#include "../include/alloc_strategy.h"
#include "../include/rec_entry.h"
#include "../include/rec_gc.h"
#include "../include/reclaim_stats.h"
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/ktime.h>
#include <linux/writeback.h>

/*
 * Deferred frees - the write barrier for copy-on-write.
 *
 * A mutation builds a shadow of the tree and then publishes a new root, but the
 * new root only becomes durable when its owner (the superblock, for the inode
 * and recovery trees; the inode itself, for the per-inode trees) is written
 * out. Handing the replaced blocks straight back to the allocator opens a
 * window: they get reused and overwritten while the *on-disk* tree still points
 * at them, so a crash before the root is persisted leaves a tree referencing
 * blocks that now belong to somebody else.
 *
 * So the blocks are parked here instead, and only released at a commit point,
 * after everything dirty has reached the disk.
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
    // Out of memory. Leaking the block until the next mkfs is ugly but safe;
    // freeing it here would break the barrier, which is not.
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

  // Snapshot first. Only blocks already on the list are covered by the
  // writeback below; anything deferred while it runs belongs to a root that
  // has not been written yet, and stays queued for the next commit.
  mutex_lock(&sbi->pending_free_lock);
  count  = sbi->pending_free_count;
  blocks = sbi->pending_free;

  sbi->pending_free       = NULL;
  sbi->pending_free_count = 0;
  sbi->pending_free_cap   = 0;

  if (writeback)
    sbi->writeback_error = 0;

  mutex_unlock(&sbi->pending_free_lock);

  // Per-inode roots only reach the inode tree through ->write_inode, so
  // without this pass the barrier below covers the trees but not the pointers
  // to them. Callers that already ran it (->sync_fs, ->put_super) skip it.
  if (writeback)
    sync_inodes_sb(sb);

  // The barrier runs even with nothing to free: callers rely on it to publish
  // the roots, not just to recycle blocks. Shadow nodes and the bitmap first,
  // then the superblock that carries the inode/recovery roots, then the device
  // cache.
  //
  // Note the node-before-superblock part is best-effort: both live in the bdev
  // mapping, so sync_blockdev() writes them in whatever order it likes. Making
  // it strict means keeping sbi->bh clean until commit time, which is a bigger
  // change than this one.
  ret = sync_blockdev(sb->s_bdev);

  if (sbi->bh) {
    err = sync_dirty_buffer(sbi->bh);

    if (!ret)
      ret = err;
  }

  err = blkdev_issue_flush(sb->s_bdev);

  if (!ret)
    ret = err;

  // A failed ->write_inode is the dangerous case, not an obvious one: the
  // inode still carries the new root in memory, so the tree on disk is the old
  // one and these blocks are exactly what it points at. Under ENOSPC that
  // failure is routine, since persisting an inode has to allocate.
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

  // Only now are the replaced blocks safe to hand out again.
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
 * True when the next mutation might not find a block.
 *
 * Under copy-on-write even a delete has to allocate before it can release, so a
 * filesystem whose free blocks are all parked on the deferred list cannot
 * delete its way out of being full. Entry points that are about to mutate a
 * tree check this first and commit while they still can - waiting for the
 * allocation to actually fail leaves the operation with an error it cannot
 * usefully report.
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

  // nothing parked, but the recovery snapshots may still be sitting on the
  // blocks of every file that was ever deleted
  return pending || sbi->super->recovery_root != 0;
}

/*
 * Writes back the dirty inodes, then commits.
 *
 * Callers must hold no filesystem locks - this waits for writeback of every
 * dirty inode. s_umount is a trylock on purpose: sync_inodes_sb() wants it, and
 * if it is already held we are being unmounted or frozen, both of which commit
 * on their own.
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

  // Still short after committing everything that was already free: what is
  // left is what the recovery snapshots are holding. Deleting a file parks its
  // blocks in one deliberately, so this is the only thing that ever gives that
  // space back - and only here, where the alternative is failing the write.
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

// defined below, next to the other single-block paths
static __u32 __allocate_single_block(struct super_block *sb, __u32 floor);

// caller must hold sbi->bitmap_lock
static void __release_run(struct super_block *sb, __u32 start, __u32 length) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  for (__u32 i = 0; i < length; i++) {
    if (bsfs_clear_bit(sb, start + i)) {
      sbi->free_blocks++;
      BSFS_STAT_INC(sb, frees);
      bsfs_alloc_strategy_freed(sb, start + i);
    }
  }
}

// releases every span this call appended to the queue, and unlinks them
// caller must hold sbi->bitmap_lock
static void __rollback_bspan_queue(struct super_block *sb, bspan_queue_t *queue, bspan_node_t *saved_rear) {
  bspan_node_t *node = saved_rear ? saved_rear->next : queue->front;

  while (node) {
    bspan_node_t *next = node->next;

    __release_run(sb, node->bspan.disk_blk, node->bspan.length);
    kfree(node);
    node = next;
  }

  if (saved_rear) {
    saved_rear->next = NULL;
    queue->rear      = saved_rear;
  } else {
    queue->front = NULL;
    queue->rear  = NULL;
  }
}

/*
 * Fills a queue with blocks_needed blocks, coalesced into as few spans as they
 * happen to form.
 *
 * This used to be a first-fit run scanner that walked the bitmap and set the
 * bits itself, which meant it allocated behind the allocator's back. That is
 * harmless with a linear scan - it re-reads the bitmap every time - but fatal
 * with any strategy that keeps an index: blocks taken here would never be
 * recorded as taken, and the index would hand them out a second time. So it goes
 * through __allocate_single_block() like everything else now.
 *
 * Contiguity is best-effort as a result, rather than first-fit guaranteed. No
 * caller depends on it: there is none in the module today, and a bspan *queue*
 * is a list of spans precisely because one span may not be enough.
 *
 * Runs are closed at U16_MAX because bspan_t.length is a __u16. An open run has
 * not been enqueued yet, so it is released directly rather than by the rollback,
 * which only walks the queue.
 *
 * `offset` is accepted for source compatibility and ignored - where blocks come
 * from is the strategy's decision now.
 */
int allocate_bspan_queue(struct super_block *sb, bspan_queue_t *queue, __u32 blocks_needed, __u32 offset) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  bspan_node_t           *saved_rear;
  __u32                   run_start  = INVALID_BLOCK;
  __u32                   run_length = 0;
  int                     ret        = 0;

  (void)offset;

  if (blocks_needed == 0)
    return 0;

  mutex_lock(&sbi->bitmap_lock);
  saved_rear = queue->rear;

  for (__u32 done = 0; done < blocks_needed; done++) {
    __u32 block = __allocate_single_block(sb, sbi->reserved_blocks);

    if (block == INVALID_BLOCK) {
      ret = -1;
      break;
    }

    if (run_start != INVALID_BLOCK && block == run_start + run_length && run_length < U16_MAX) {
      run_length++;
      continue;
    }

    if (run_start != INVALID_BLOCK && enqueue_bspan(queue, 0, run_start, (__u16)run_length) != 0) {
      __release_run(sb, run_start, run_length);
      __release_run(sb, block, 1);
      run_start = INVALID_BLOCK;
      ret       = -ENOMEM;
      break;
    }

    run_start  = block;
    run_length = 1;
  }

  if (run_start != INVALID_BLOCK) {
    if (ret != 0 || enqueue_bspan(queue, 0, run_start, (__u16)run_length) != 0) {
      __release_run(sb, run_start, run_length);

      if (ret == 0)
        ret = -ENOMEM;
    }
  }

  if (ret != 0)
    __rollback_bspan_queue(sb, queue, saved_rear);

  mutex_unlock(&sbi->bitmap_lock);

  return ret;
}

/*
 * Counts the free blocks once, at mount, and sizes the metadata reserve.
 *
 * The reserve exists because copy-on-write inverts the usual assumption that
 * releasing space is free: btree_remove() has to clone a root-to-leaf path
 * before it can drop anything, so a filesystem that is completely full cannot
 * be truncated or unlinked back into shape. Data allocations stop short of the
 * reserve; metadata ones may spend it.
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

  // a couple of full COW paths' worth, capped so tiny images stay usable
  usable  = sbi->super->total_blocks - sbi->super->data_block_start;
  reserve = usable / 64;

  if (reserve > BSFS_META_RESERVE)
    reserve = BSFS_META_RESERVE;

  sbi->reserved_blocks = reserve;

  mutex_unlock(&sbi->bitmap_lock);
}

/*
 * The one place a block is chosen.
 *
 * The floor is the reserve check and stays here, above the strategy: how deep
 * into the metadata reserve an allocation may dig is a policy question about the
 * caller (see bsfs_alloc_prio_t), not about how free blocks are indexed. The
 * strategy below only answers "which free block", and src/alloc_linear.c still
 * answers it exactly the way this function used to.
 *
 * A strategy that finds nothing while free_blocks says otherwise has drifted out
 * of sync with the bitmap. The strategy is believed: handing out a block it does
 * not know about is what corrupts data.
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
 * Allocates a block for file data. Stops at the metadata reserve, so there is
 * always enough room left for a truncate or an unlink to shrink the trees.
 */
__u32 allocate_single_block(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   block;

  mutex_lock(&sbi->bitmap_lock);
  block = __allocate_single_block(sb, sbi->reserved_blocks);
  mutex_unlock(&sbi->bitmap_lock);

  if (block != INVALID_BLOCK)
    return block;

  // Out of space here often just means the deferred frees are holding blocks
  // back, but this is the wrong place to release them: the caller is usually
  // mid-transaction, holding truncate_mutex or inode_tree_lock, and the barrier
  // needs a writeback pass it cannot run from here. Fail, and let the VFS entry
  // point retry through bsfs_reclaim_space().
  pr_warn_ratelimited("no blocks available (%zu deferred)\n", sbi->pending_free_count);

  return INVALID_BLOCK;

  // GC on-demand for at least 1 block, then retry once
  // TODO: reenable
  // (void)rec_gc_purge_until(sb, 1, (__u64)ktime_get_real_seconds());

  // return allocate_single_block(sb);
}

/*
 * Allocates a block for tree metadata, and may spend the reserve.
 *
 * Every copy-on-write clone and node split goes through here. Shrinking the
 * tree is the only way a full filesystem gets better, and shrinking allocates,
 * so this path must not be the one that stops first.
 */
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
  // rollback paths can reach the same block twice; only a real transition counts
  if (bsfs_clear_bit(sb, block_index)) {
    sbi->free_blocks++;
    BSFS_STAT_INC(sb, frees);
    bsfs_alloc_strategy_freed(sb, block_index);
  }
  mutex_unlock(&sbi->bitmap_lock);
  return 0;
}

/*
 * Which allocator this module was built with.
 *
 * One line per mount, unconditionally: with the strategy chosen at build time
 * there is otherwise no way to tell from a running system - or from a test log -
 * which .ko is loaded, and "the benchmark ran against the wrong build" is a
 * mistake that does not announce itself.
 *
 * The counter keys in the unmount summary are stable and space-separated so
 * tests/alloc_guest.sh can pull them out of dmesg with sed.
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

int deallocate_bspan(struct super_block *sb, const bspan_t *bspan) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  // Clear bits corresponding to the span
  mutex_lock(&sbi->bitmap_lock);
  for (__u32 i = 0; i < bspan->length; i++) {
    if (bsfs_clear_bit(sb, bspan->disk_blk + i)) {
      sbi->free_blocks++;
      BSFS_STAT_INC(sb, frees);
      bsfs_alloc_strategy_freed(sb, bspan->disk_blk + i);
    }
  }
  mutex_unlock(&sbi->bitmap_lock);

  return 0;
}
