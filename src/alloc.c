/**
 * @file allocate_bsfs.c
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#include "../include/alloc.h"
#include "../include/rec_gc.h"

int allocate_bspan_queue(struct super_block *sb, bspan_queue_t *queue, __u32 blocks_needed, __u32 offset) {
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
          bsfs_set_bit(sb, j);
        }
        enqueue_bspan(queue, 0, run_start, run_length);
        return 0;
      }
    } else if (run_length > 0) {
      for (__u32 j = run_start; j < run_start + run_length; j++) {
        bsfs_set_bit(sb, j);
      }
      enqueue_bspan(queue, 0, run_start, run_length);
      int result = allocate_bspan_queue(sb, queue, blocks_needed - run_length, i + 1);
      return result;
    }
  }

  // Final run if reached end of bitmap with a partial span
  if (run_length > 0) {
    for (__u32 j = run_start; j < run_start + run_length; j++) {
      bsfs_set_bit(sb, j);
    }
    enqueue_bspan(queue, 0, run_start, run_length);
    int result = allocate_bspan_queue(sb, queue, blocks_needed - run_length, sbi->super->total_blocks);
    return result;
  }

  return -1;

  // TODO: reenable
  // On-demand GC: try to free enough blocks and retry once
  // (void)rec_gc_purge_until(sb, blocks_needed, (__u64)ktime_get_real_seconds());

  // Retry
  // return allocate_bspan_queue(sb, queue, blocks_needed, offset);
}

__u32 allocate_single_block(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 i;
  __u32 start        = sbi->super->data_block_start;
  __u32 total_blocks = sbi->super->total_blocks;
  for (i = start; i < total_blocks; i++) {
    if (bsfs_is_bit_free(sb, i)) {
      bsfs_set_bit(sb, i);
      return i;
    }
  }

  printk(KERN_WARNING "Error: No blocks available.\n");

  return INVALID_BLOCK;

  // GC on-demand for at least 1 block, then retry once
  // TODO: reenable
  // (void)rec_gc_purge_until(sb, 1, (__u64)ktime_get_real_seconds());

  // return allocate_single_block(sb);
}

int deallocate_single_block(struct super_block *sb, __u32 block_index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  if (block_index >= sbi->super->total_blocks) {
    printk(KERN_ERR "Invalid block index for deallocation: %u\n", block_index);
    return -1;
  }

  bsfs_clear_bit(sb, block_index);
  return 0;
}

int deallocate_bspan(struct super_block *sb, const bspan_t *bspan) {
  // Clear bits corresponding to the span
  for (__u32 i = 0; i < bspan->length; i++) {
    bsfs_clear_bit(sb, bspan->disk_blk + i);
  }

  return 0;
}
