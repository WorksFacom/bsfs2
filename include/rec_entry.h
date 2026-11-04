/**
 * @file rec_entry.h
 * @brief The recovery tree: snapshots of deleted files, kept for restore.
 *
 * Deleting a file does not free its blocks. bsfs_evict_inode() records a
 * rec_entry_t that takes over the file's tree, and the space only comes back
 * when bsfs_rec_tree_purge() drops the snapshot. Snapshots are keyed by
 * recovery_id. The root lives in super->recovery_root, 0 while the tree is
 * empty. sbi->rec_lock serializes every access.
 */

#ifndef REC_ENTRY_OPERATIONS_BSFS_H
#define REC_ENTRY_OPERATIONS_BSFS_H

#include "../bsfs.h"

/**
 * @brief Sets up the in-memory recovery state at mount.
 *
 * Only initializes rec_lock and the id allocator. The tree itself is read
 * lazily through super->recovery_root.
 */
void bsfs_rec_tree_mount_init(struct super_block *sb);

/**
 * @brief Adds a snapshot and publishes the new recovery_root.
 *
 * A recovery_id of 0 gets the next free id. A retention_until of 0 gets
 * deleted_at + DEFAULT_RECOVERY_TTL, if deleted_at is set. Both are written
 * back into @p entry. The
 * caller holds sbi->rec_lock.
 *
 * @return 0, or -EIO if the insert kept failing.
 */
int bsfs_rec_tree_add(struct super_block *sb, btree_handle_t *rec_tree, rec_entry_t *entry);

/**
 * @brief Removes the snapshot with @p recovery_id and publishes the new root.
 *
 * Only drops the entry. The file tree it pointed at is the caller's problem,
 * either handed back to a restored file or deferred with
 * btree_deallocate_all(). The caller holds sbi->rec_lock.
 *
 * @return 1 if removed, 0 if missing or the removal failed.
 */
int bsfs_rec_tree_remove(struct super_block *sb, btree_handle_t *rec_tree, __u32 recovery_id);

/**
 * @brief Drops recovery snapshots, expired or oldest first, and queues the
 *        blocks they were holding for release.
 *
 * Deleting a file parks its tree in a snapshot rather than releasing it, so
 * this is the only thing that ever gives that space back. Meant to run under
 * space pressure only, from bsfs_reclaim_space(). Takes sbi->rec_lock itself.
 *
 * @param max_entries the most snapshots to drop in one call.
 * @param blocks_deferred if not NULL, incremented by the number of blocks queued
 *                        for release.
 * @return the number of entries purged.
 */
unsigned int bsfs_rec_tree_purge(struct super_block *sb, unsigned int max_entries, __u64 *blocks_deferred);

#endif /* REC_ENTRY_OPERATIONS_BSFS_H */
