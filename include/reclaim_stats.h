#ifndef BSFS_RECLAIM_STATS_H
#define BSFS_RECLAIM_STATS_H

#include "../bsfs.h"

int bsfs_reclaim_debugfs_init(void);
void bsfs_reclaim_debugfs_exit(void);
void bsfs_reclaim_stats_mount(struct super_block *sb);
void bsfs_reclaim_stats_unmount(struct super_block *sb);
void bsfs_reclaim_stats_failure(struct super_block *sb, int error);

#endif
