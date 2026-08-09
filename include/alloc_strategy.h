/**
 * @file alloc_strategy.h
 * @brief The pluggable block-allocation strategy interface.
 *
 * Everything about allocating blocks in BSFS is strategy-independent except one
 * question: given the bitmap, which free block do we hand out next? The linear
 * scan that used to answer it lives in src/alloc_linear.c now; src/alloc_heap.c
 * answers it with a min-heap, and further strategies (two-level index, red-black
 * tree) plug in the same way.
 *
 * Exactly one implementation is compiled into the module, chosen at build time:
 *
 *   make                    # BSFS_ALLOC=linear, the historical behaviour
 *   make BSFS_ALLOC=heap    # min-heap
 *
 * so these are plain extern functions rather than a vtable - the linker picks
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
 * The implementation must mark the block with bsfs_set_bit() - that is what
 * dirties the bitmap buffer - and must NOT touch sbi->free_blocks; the caller
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
 * That guarantee matters: the rollback paths in btree.c and inode.c can reach
 * the same block twice, and a strategy that recorded it twice would hand it out
 * to two different owners.
 */
void bsfs_alloc_strategy_freed(struct super_block *sb, __u32 block);

/**
 * @brief Logs which strategy the module was built with, once per mount.
 */
void bsfs_alloc_log_mount(struct super_block *sb);

#endif // BSFS_ALLOC_STRATEGY_H
