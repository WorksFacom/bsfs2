/**
 * @file allocate_bsfs.c
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#include "../include/alloc.h"
#include "../include/rec_entry.h"
#include "../include/rec_gc.h"
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
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
    sbi->writeback_failed = false;

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
  if (!ret && sbi->writeback_failed)
    ret = -EIO;

  if (ret) {
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
  int ret;

  if (!down_read_trylock(&sb->s_umount))
    return -EBUSY;

  ret = __commit_deferred_frees(sb, true);

  // Still short after committing everything that was already free: what is
  // left is what the recovery snapshots are holding. Deleting a file parks its
  // blocks in one deliberately, so this is the only thing that ever gives that
  // space back - and only here, where the alternative is failing the write.
  if (ret == 0 && low_on_space(sb) && bsfs_rec_tree_purge(sb, BSFS_PURGE_BATCH) > 0)
    ret = __commit_deferred_frees(sb, true);

  up_read(&sb->s_umount);

  return ret;
}

// caller must hold sbi->bitmap_lock
static int __allocate_bspan_queue(struct super_block *sb, bspan_queue_t *queue, __u32 blocks_needed, __u32 offset) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  if (blocks_needed == 0 || offset >= sbi->super->total_blocks) {
    return (blocks_needed == 0) ? 0 : -1;
  }

  __u32 run_start  = INVALID_BLOCK;
  __u32 run_length = 0;

  if (offset < sbi->super->data_block_start)
    offset = sbi->super->data_block_start;
  for (__u32 i = offset; i < sbi->super->total_blocks; i++) {
    if (bsfs_is_bit_free(sb, i)) {
      if (run_start == INVALID_BLOCK) {
        run_start  = i;
        run_length = 1;
      } else {
        run_length++;
      }

      if (run_length == blocks_needed) {
        for (__u32 j = run_start; j < run_start + run_length; j++) {
          if (bsfs_set_bit(sb, j))
            sbi->free_blocks--;
        }
        enqueue_bspan(queue, 0, run_start, run_length);
        return 0;
      }
    } else if (run_length > 0) {
      for (__u32 j = run_start; j < run_start + run_length; j++) {
        if (bsfs_set_bit(sb, j))
          sbi->free_blocks--;
      }
      enqueue_bspan(queue, 0, run_start, run_length);
      int result = __allocate_bspan_queue(sb, queue, blocks_needed - run_length, i + 1);
      return result;
    }
  }

  // Final run if reached end of bitmap with a partial span
  if (run_length > 0) {
    for (__u32 j = run_start; j < run_start + run_length; j++) {
      if (bsfs_set_bit(sb, j))
        sbi->free_blocks--;
    }
    enqueue_bspan(queue, 0, run_start, run_length);
    int result = __allocate_bspan_queue(sb, queue, blocks_needed - run_length, sbi->super->total_blocks);
    return result;
  }

  return -1;

  // TODO: reenable
  // On-demand GC: try to free enough blocks and retry once
  // (void)rec_gc_purge_until(sb, blocks_needed, (__u64)ktime_get_real_seconds());

  // Retry
  // return allocate_bspan_queue(sb, queue, blocks_needed, offset);
}

int allocate_bspan_queue(struct super_block *sb, bspan_queue_t *queue, __u32 blocks_needed, __u32 offset) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  int                     ret;

  mutex_lock(&sbi->bitmap_lock);
  ret = __allocate_bspan_queue(sb, queue, blocks_needed, offset);
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

// caller must hold sbi->bitmap_lock
static __u32 __allocate_single_block(struct super_block *sb, __u32 floor) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 i;
  __u32 start        = sbi->super->data_block_start;
  __u32 total_blocks = sbi->super->total_blocks;

  if (sbi->free_blocks <= floor)
    return INVALID_BLOCK;

  for (i = start; i < total_blocks; i++) {
    if (bsfs_is_bit_free(sb, i)) {
      if (bsfs_set_bit(sb, i))
        sbi->free_blocks--;
      return i;
    }
  }

  return INVALID_BLOCK;
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
  if (bsfs_clear_bit(sb, block_index))
    sbi->free_blocks++;
  mutex_unlock(&sbi->bitmap_lock);
  return 0;
}

int deallocate_bspan(struct super_block *sb, const bspan_t *bspan) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  // Clear bits corresponding to the span
  mutex_lock(&sbi->bitmap_lock);
  for (__u32 i = 0; i < bspan->length; i++) {
    if (bsfs_clear_bit(sb, bspan->disk_blk + i))
      sbi->free_blocks++;
  }
  mutex_unlock(&sbi->bitmap_lock);

  return 0;
}
