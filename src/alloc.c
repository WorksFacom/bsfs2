/**
 * @file allocate_bsfs.c
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#include "../include/alloc.h"
#include "../include/rec_gc.h"

int allocate_bspan_queue(super_block *sb, bspan_queue_t *queue, __u32 blocks_needed, __u32 offset) {
  if (blocks_needed == 0 || offset >= sb->total_blocks) {
    return (blocks_needed == 0) ? 0 : -1;
  }

  __u8 *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap) {
    fprintf(stderr, "Failed to load bitmap from disk.\n");
    return -1;
  }

  __u32 run_start  = INVALID_BLOCK;
  __u32 run_length = 0;

  if (offset < sb->data_block_start)
    offset = sb->data_block_start;
  for (__u32 i = offset; i < sb->total_blocks; i++) {
    if (bsfs_is_bit_free(bitmap, i)) {
      if (run_start == INVALID_BLOCK) {
        run_start  = i;
        run_length = 1;
      } else {
        run_length++;
      }

      if (run_length == blocks_needed) {
        for (__u32 j = run_start; j < run_start + run_length; j++) {
          bsfs_set_bit(bitmap, j);
        }
        enqueue_bspan(queue, 0, run_start, run_length);
        write_bitmap(disk, bitmap, sb->block_bitmap_start);
        free(bitmap);
        return 0;
      }
    } else if (run_length > 0) {
      for (__u32 j = run_start; j < run_start + run_length; j++) {
        bsfs_set_bit(bitmap, j);
      }
      enqueue_bspan(queue, 0, run_start, run_length);
      int result = allocate_bspan_queue(disk, sb, queue, blocks_needed - run_length, i + 1);
      write_bitmap(disk, bitmap, sb->block_bitmap_start);
      free(bitmap);
      return result;
    }
  }

  // Final run if reached end of bitmap with a partial span
  if (run_length > 0) {
    for (__u32 j = run_start; j < run_start + run_length; j++) {
      bsfs_set_bit(bitmap, j);
    }
    enqueue_bspan(queue, 0, run_start, run_length);
    int result = allocate_bspan_queue(disk, sb, queue, blocks_needed - run_length, sb->total_blocks);
    write_bitmap(disk, bitmap, sb->block_bitmap_start);
    free(bitmap);
    return result;
  }

  free(bitmap);
  // On-demand GC: try to free enough blocks and retry once
  (void)rec_gc_purge_until(disk, sb, blocks_needed, (__u64)time(NULL));

  // Retry
  return allocate_bspan_queue(disk, sb, queue, blocks_needed, offset);
}

__u32 allocate_single_block(super_block *sb) {
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

  fprintf(stderr, "Error: No blocks available.\n");
  // GC on-demand for at least 1 block, then retry once
  (void)rec_gc_purge_until(disk, sb, 1, (__u64)time(NULL));

  return allocate_single_block(sb);
}

int deallocate_single_block(super_block *sb, __u32 block_index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  if (block_index >= sbi->super->total_blocks) {
    fprintf(stderr, "Invalid block index for deallocation: %u\n", block_index);
    return -1;
  }

  bsfs_clear_bit(sb, block_index);
  return 0;
}

int deallocate_bspan(super_block *sb, const bspan_t *bspan) {
  // Clear bits corresponding to the span
  for (__u32 i = 0; i < bspan->length; i++) {
    bsfs_clear_bit(sb, bspan->disk_blk + i);
  }

  return 0;
}
