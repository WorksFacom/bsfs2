/**
 * @file bitmap.h
 * @brief The block bitmap, pinned in memory for the whole mount.
 *
 * On disk the bitmap is a chain of bitmap_block_t, linked through next_block.
 * load_bitmap() reads every block once and keeps its buffer head, so
 * sbi->bitmap[i] points straight into the page cache and a set or clear only has
 * to mark the buffer dirty. Bit n covers block n. Bits are LSB-first inside a
 * byte, which is the little-endian layout the kernel's *_le bitops expect.
 *
 * The inline accessors do no locking and no bounds checks. Callers hold
 * sbi->bitmap_lock and keep the index below total_blocks.
 */
#ifndef BITMAP_OPERATIONS
#define BITMAP_OPERATIONS

#include "../bsfs.h"
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/types.h>

/**
 * @brief Reads the bitmap chain and pins every block of it.
 *
 * Walks next_block from block_bitmap_start for block_bitmap_total hops, and
 * refuses any link outside that range. The hop count is checked against
 * total_blocks first. Both values come off the disk, and the bit accessors
 * index the result without bounds checks.
 *
 * @return 0, -EUCLEAN for a bitmap that is too short or a bad link, -ENOMEM, or
 *         -EIO. On failure the caller must still call release_bitmap() to drop
 *         whatever was loaded.
 */
int load_bitmap(struct super_block *sb);

/**
 * @brief Drops the buffer heads and arrays load_bitmap() pinned.
 *
 * Flushes any bitmap block still dirty first. Safe after a failed load_bitmap().
 */
void release_bitmap(struct super_block *sb);

/**
 * @brief Tests one bit.
 *
 * @return 1 if block @p index is free, 0 if it is allocated.
 */
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
 * @brief Marks block @p index allocated and dirties its bitmap buffer.
 *
 * @return true if the bit went from 0 to 1, false if it was already set. The
 *         caller adjusts sbi->free_blocks only on true.
 */
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
 * @brief Marks block @p index free and dirties its bitmap buffer.
 *
 * The rollback paths can free the same block twice, so the return value is what
 * keeps the free count straight.
 *
 * @return true if the bit went from 1 to 0, false if it was already clear.
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
 * @brief Finds the first free block in [from, to), or returns @p to if there is
 *        none.
 *
 * The bit-at-a-time alternative is a loop over bsfs_is_bit_free(), which redoes
 * the block/byte division on every bit. This walks a machine word at a time
 * through find_next_zero_bit_le(), so a full bucket or a long run of allocated
 * blocks is skipped 64 bits per operation instead of one.
 *
 * The `_le` variant is the correct one because bsfs packs bits LSB-first inside
 * a byte (`1U << (index & 7)`). On x86 it compiles to the same thing as the
 * native version. On big-endian it would not.
 *
 * The search is split at bitmap-block boundaries because the bits of each one
 * live in a separate buffer head and are not contiguous in memory. Callers whose
 * range never straddles a boundary get a single iteration.
 *
 * @param from first block to consider (must be < the total block count)
 * @param to   one past the last block to consider
 * @return the block index, or @p to if every block in the range is allocated.
 */
static inline __u32 bsfs_find_free_bit(struct super_block *sb, __u32 from, __u32 to) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  const __u32             per = (__u32)BITS_PER_BLOCK;

  while (from < to) {
    __u32         block = from / per;
    __u32         base  = block * per;
    __u32         limit = (to - base < per) ? to - base : per;
    unsigned long hit;

    // the search masks bits past `limit`, so the tail of the last word (which overlaps next_block) is
    // never returned. reading it stays inside the 4096-byte buffer.
    hit = find_next_zero_bit_le(sbi->bitmap[block], limit, from - base);

    if (hit < limit)
      return base + (__u32)hit;

    from = base + per;
  }

  return to;
}
#endif // BITMAP_OPERATIONS
