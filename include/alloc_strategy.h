/**
 * @file alloc_strategy.h
 * @brief Interface every block-allocation strategy implements.
 *
 * Everything about allocating blocks in BSFS is strategy-independent except one
 * question: given the bitmap, which free block do we hand out next? The linear
 * scan that used to answer it lives in src/alloc_linear.c now; src/alloc_heap.c
 * answers it with a min-heap of recycled blocks, src/alloc_twolevel.c with a
 * free count per bucket, and further strategies (red-black tree, ...) plug in
 * the same way.
 *
 * Exactly one implementation is compiled into the module, chosen at build time:
 *
 *   make                        # BSFS_ALLOC=linear, the default
 *   make BSFS_ALLOC=heap        # min-heap
 *   make BSFS_ALLOC=twolevel    # two-level index
 *
 * so these are plain extern functions rather than a vtable. The linker picks
 * the implementation and the calls stay direct.
 *
 * ## The contract
 *
 * The bitmap in sbi->bitmap[] is the authority on what is allocated; it lives in
 * buffer heads the kernel can write back at any moment. A strategy's own
 * structures are a *derived index* over it, never the record of truth: they are
 * built at mount and thrown away at unmount, and nothing about them reaches the
 * disk.
 *
 * The caller (src/alloc.c) owns:
 *   - sbi->bitmap_lock, held across every _alloc()/_freed() call
 *   - sbi->free_blocks and sbi->reserved_blocks, including the reserve floor
 *     that decides whether an allocation is allowed at all
 *
 * The strategy owns only the choice of block, and the bit it sets for it.
 */
#ifndef BSFS_ALLOC_STRATEGY_H
#define BSFS_ALLOC_STRATEGY_H

#include "../bsfs.h"
#include <linux/fs.h>
#include <linux/types.h>

#ifndef BSFS_ALLOC_STRATEGY_NAME
#define BSFS_ALLOC_STRATEGY_NAME "linear"
#endif

/**
 * @brief Builds whatever index the strategy needs, at mount time.
 *
 * Called from bsfs_fill_super() after load_bitmap() and bsfs_init_block_counts(),
 * so the bitmap is readable and sbi->free_blocks is already correct. May store
 * private state in sbi->alloc_private.
 *
 * @return 0, or a negative error (the mount is then failed).
 */
int bsfs_alloc_strategy_init(struct super_block *sb);

/**
 * @brief Releases everything _init() built. Idempotent, and safe to call after a
 *        failed _init() or on a partially set up superblock.
 *
 * Called before release_bitmap(), so the bitmap is still readable here.
 */
void bsfs_alloc_strategy_destroy(struct super_block *sb);

/**
 * @brief Picks a free block, marks it allocated, and returns it.
 *
 * Caller holds sbi->bitmap_lock and has already checked the reserve floor.
 *
 * The implementation must mark the block with bsfs_set_bit(), since that is what
 * dirties the bitmap buffer. It must NOT touch sbi->free_blocks. The caller
 * adjusts it based on the return value.
 *
 * @return the block index, or INVALID_BLOCK if nothing is available.
 */
__u32 bsfs_alloc_strategy_alloc(struct super_block *sb);

/**
 * @brief Tells the strategy a block became free.
 *
 * Caller holds sbi->bitmap_lock and calls this only after a real 1->0 transition
 * of the block's bit, i.e. when bsfs_clear_bit() returned true.
 *
 * That guarantee matters. The rollback paths in btree.c and inode.c can reach
 * the same block twice, and a strategy that recorded it twice would hand it out
 * to two different owners.
 */
void bsfs_alloc_strategy_freed(struct super_block *sb, __u32 block);

/**
 * @brief One line about the strategy's state, for the unmount summary.
 *
 * Only ever called under BSFS_ALLOC_STATS. Writes at most @p len bytes into @p buf.
 * A strategy with nothing to report may write an empty string.
 */
void bsfs_alloc_strategy_stats(struct super_block *sb, char *buf, size_t len);

/*
 * Allocation counters.
 *
 * These exist so a test can tell that a strategy is actually engaged, instead of
 * only that the filesystem did not break. "The min-heap recycled 9871 blocks"
 * is an assertion, "no test failed" is not. Off by default, since the counters
 * sit on the allocation hot path:
 *
 *   make BSFS_ALLOC=heap BSFS_ALLOC_STATS=y
 *
 * Guarded rather than always-on because with them compiled out the accounting
 * disappears entirely, leaving the exact code the benchmarks should measure.
 */
#ifdef BSFS_ALLOC_STATS
#define BSFS_STAT_INC(sb, field)                                                                                                           \
  do {                                                                                                                                     \
    bsfs_sb(sb)->alloc_stats.field++;                                                                                                      \
  } while (0)
#define BSFS_STAT_MAX(sb, field, value)                                                                                                    \
  do {                                                                                                                                     \
    typeof(value) __v = (value);                                                                                                           \
    if (__v > bsfs_sb(sb)->alloc_stats.field)                                                                                              \
      bsfs_sb(sb)->alloc_stats.field = __v;                                                                                                \
  } while (0)
#else
#define BSFS_STAT_INC(sb, field)                                                                                                           \
  do {                                                                                                                                     \
  } while (0)
#define BSFS_STAT_MAX(sb, field, value)                                                                                                    \
  do {                                                                                                                                     \
  } while (0)
#endif

/**
 * @brief Logs the strategy name at mount, and the counters at unmount.
 *
 * The name line is printed on every mount. The counters only exist under
 * BSFS_ALLOC_STATS.
 */
void bsfs_alloc_log_mount(struct super_block *sb);
void bsfs_alloc_log_unmount(struct super_block *sb);

#endif // BSFS_ALLOC_STRATEGY_H
