/**
 * @file bitmap_operations_bsfs.h
 * @brief Bitmap management utilities for the BSFS file system.
 *
 * This header provides the core functions to manage the block allocation bitmap
 * in the Basic Solution File System (BSFS). The bitmap tracks which blocks on
 * the disk are allocated or free, and is essential for operations such as:
 *
 * - Block allocation and deallocation
 * - File creation and deletion
 * - Space management and consistency
 *
 * The bitmap is stored on disk as a linked list of fixed-size blocks and is
 * loaded into memory for manipulation during runtime. These functions abstract
 * the bitmap I/O and provide low-level access to set, clear, and query bit
 * status.
 *
 * ## Key functionalities:
 * - Initialization of the bitmap block list on disk
 * - Loading and saving the bitmap from/to disk
 * - Marking individual blocks as free or occupied
 * - Querying the state of specific blocks
 *
 * This interface is used internally by the allocator and file system logic
 * to ensure efficient space tracking and integrity of data block usage.
 */
#ifndef BITMAP_OPERATIONS
#define BITMAP_OPERATIONS

#include "../bsfs.h"
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/types.h>

/**
 * @brief Loads a bitmap from the specified file.
 *
 * This function reads the bitmap from the disk, starting at the given position
 * and using the block size to determine the number of bytes to read.
 *
 * The chain is walked under the block count the superblock declares, and that
 * count is checked against the size of the filesystem first: both come off the
 * disk, and the bit accessors index the resulting array without bounds checks.
 *
 * @param disk The file pointer to the filesystem image.
 * @param bitmap_start_block The block where the bitmap starts.
 * @return 0 on success, or a negative error. On failure the caller must still
 *         call release_bitmap() to drop whatever was loaded.
 */
int load_bitmap(struct super_block *sb);

/**
 * @brief Releases everything load_bitmap() pinned (buffer heads and arrays).
 *
 * Flushes any bitmap block still dirty before dropping it.
 */
void release_bitmap(struct super_block *sb);

/**
 * @brief Checks if a given block is free in the bitmap.
 *
 * This function checks the status of a specific block in the bitmap to
 * determine if it is free (available for allocation) or occupied.
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to check.
 * @return 1 if the block is free, 0 if the block is occupied.
 */
// TODO: are these 3 functions below working??
static inline __u32 bsfs_is_bit_free(struct super_block *sb, __u32 index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 byte_index = index >> 3;
  __u32 block      = byte_index / BITMAP_BLOCK_BYTES;
  __u32 byte       = byte_index % BITMAP_BLOCK_BYTES;
  __u8  mask       = (__u8)(1U << (index & 7));
  __u8 *bits       = sbi->bitmap[block];

  return (bits[byte] & mask) == 0;
}

/**
 * @brief Marks a block as allocated in the bitmap.
 *
 * This function sets the bit corresponding to a given block to indicate that it
 * is now allocated (occupied).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as allocated.
 */
// returns true when the bit actually changed, so the caller can keep the free
// count straight - the allocator clears some bits twice on its rollback paths
static inline bool bsfs_set_bit(struct super_block *sb, __u32 index) {
  // TODO: #define ext4_test_and_set_bit		__test_and_set_bit_le
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 byte_index = index >> 3;
  __u32 block      = byte_index / BITMAP_BLOCK_BYTES;
  __u32 byte       = byte_index % BITMAP_BLOCK_BYTES;
  __u8  mask       = (__u8)(1U << (index & 7));
  __u8 *bits       = sbi->bitmap[block];

  if (bits[byte] & mask)
    return false;

  bits[byte] |= mask;

  mark_buffer_dirty(sbi->bitmap_map[block]);
  return true;
}

/**
 * @brief Marks a block as free in the bitmap.
 *
 * This function clears the bit corresponding to a given block to indicate that
 * it is now free (available for allocation).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as free.
 */
static inline bool bsfs_clear_bit(struct super_block *sb, __u32 index) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 byte_index = index >> 3;
  __u32 block      = byte_index / BITMAP_BLOCK_BYTES;
  __u32 byte       = byte_index % BITMAP_BLOCK_BYTES;
  __u8  mask       = (__u8)(1U << (index & 7));
  __u8 *bits       = sbi->bitmap[block];

  if (!(bits[byte] & mask))
    return false;

  bits[byte] &= (__u8)~mask;

  mark_buffer_dirty(sbi->bitmap_map[block]);
  return true;
}

/**
 * @brief Finds the first free block in [from, to), or returns @to if there is
 *        none.
 *
 * The bit-at-a-time alternative is a loop over bsfs_is_bit_free(), which redoes
 * the block/byte division on every bit. This walks a machine word at a time
 * through find_next_zero_bit_le(), so a full bucket or a long run of allocated
 * blocks is skipped 64 bits per operation instead of one.
 *
 * The `_le` variant is the correct one: bsfs packs bits LSB-first inside a byte
 * (`1U << (index & 7)`, see above), which is the little-endian on-disk bitmap
 * convention the kernel provides these accessors for. On x86 it compiles to the
 * same thing as the native version; on big-endian it would not.
 *
 * The search is split at bitmap-block boundaries because the bits of each one
 * live in a separate buffer head and are not contiguous in memory. Callers whose
 * range never straddles a boundary get a single iteration.
 *
 * @param from first block to consider (must be < the total block count)
 * @param to   one past the last block to consider
 * @return the block index, or @to if every block in the range is allocated.
 */
static inline __u32 bsfs_find_free_bit(struct super_block *sb, __u32 from, __u32 to) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  const __u32             per = (__u32)BITS_PER_BLOCK;

  while (from < to) {
    __u32         block = from / per;
    __u32         base  = block * per;
    __u32         limit = (to - base < per) ? to - base : per;
    unsigned long hit;

    // Bits past `limit` are masked off by the search itself, so the tail of the
    // last word - which overlaps bitmap_block_t.next_block - can never be
    // returned as a free block. Reading it stays inside the 4096-byte buffer.
    hit = find_next_zero_bit_le(sbi->bitmap[block], limit, from - base);

    if (hit < limit)
      return base + (__u32)hit;

    from = base + per;
  }

  return to;
}
#endif // BITMAP_OPERATIONS
