/**
 * @file reclaim_stats.h
 * @brief Per-mount counters for bsfs_reclaim_space(), exposed through debugfs.
 *
 * Each mount gets /sys/kernel/debug/bsfs/\<dev\>/reclaim_stats with the
 * attempts, successes, entries_purged and blocks_reclaimed counters, the time
 * spent in each phase (pre_commit_ns, purge_ns, post_commit_ns), failures, and
 * one error_code_\<errno\> line per distinct error seen.
 *
 * Debugfs trouble never fails a mount. If the stats cannot be allocated, that
 * mount collects none.
 */
#ifndef BSFS_RECLAIM_STATS_H
#define BSFS_RECLAIM_STATS_H

#include "../bsfs.h"

/**
 * @brief Creates the top-level bsfs debugfs directory at module load.
 *
 * @return 0, also when debugfs is not available, or a negative error.
 */
int bsfs_reclaim_debugfs_init(void);

/** @brief Removes the bsfs debugfs tree at module unload. */
void bsfs_reclaim_debugfs_exit(void);

/** @brief Allocates this mount's stats and creates its reclaim_stats file. */
void bsfs_reclaim_stats_mount(struct super_block *sb);

/**
 * @brief Removes the file and drops the mount's reference to the stats.
 *
 * The stats are refcounted, so a reader that still has the file open keeps
 * them alive past unmount.
 */
void bsfs_reclaim_stats_unmount(struct super_block *sb);

/**
 * @brief Counts a failed reclaim, bucketed by @p error.
 *
 * Only the first BSFS_RECLAIM_ERROR_SLOTS distinct error codes get a bucket.
 * Later ones still bump failures.
 */
void bsfs_reclaim_stats_failure(struct super_block *sb, int error);

#endif
