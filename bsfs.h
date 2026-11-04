/**
 * @file bsfs.h
 * @brief In-memory state of a mounted bsfs: per-mount info, the inode wrapper and tree handles.
 *
 * Nothing in here reaches the disk. The on-disk structs are in include/bsfs_shared.h.
 */
#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include "include/bsfs_shared.h"
#include <linux/atomic.h>
#include <linux/buffer_head.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/// seconds a recovery snapshot is kept, 30 days. expired ones go first, but only under space pressure
#define DEFAULT_RECOVERY_TTL (30ULL * 24ULL * 60ULL * 60ULL)

/// distinct errno buckets per mount in bsfs_reclaim_stats
#define BSFS_RECLAIM_ERROR_SLOTS 16

/** @brief One errno bucket of bsfs_reclaim_stats. */
struct bsfs_reclaim_error {
  int   error; ///< negative errno
  __u64 count; ///< failures with that errno, 0 marks a free slot
};

/**
 * @brief Counters behind /sys/kernel/debug/bsfs/\<dev\>/reclaim_stats.
 *
 * See reclaim_stats.h. Times are nanoseconds summed over every call.
 */
struct bsfs_reclaim_stats {
  struct kref               references;                       ///< held by the mount and by every open reader
  atomic64_t                attempts;                         ///< calls to bsfs_reclaim_space()
  atomic64_t                successes;                        ///< calls that purged snapshots and committed them
  atomic64_t                entries_purged;                   ///< snapshots dropped
  atomic64_t                blocks_reclaimed;                 ///< blocks queued by the purges of successful calls
  atomic64_t                pre_commit_ns;                    ///< time in the first commit
  atomic64_t                purge_ns;                         ///< time in bsfs_rec_tree_purge()
  atomic64_t                post_commit_ns;                   ///< time in the commit after a purge
  atomic64_t                failures;                         ///< calls that returned an error, -EBUSY included
  spinlock_t                error_lock;                       ///< guards errors
  struct bsfs_reclaim_error errors[BSFS_RECLAIM_ERROR_SLOTS]; ///< failures per errno
  struct dentry            *debugfs_dir;                      ///< this mount's debugfs directory, NULL without debugfs
};

#ifdef BSFS_ALLOC_STATS
/**
 * allocator counters for tests and benchmarks, only with BSFS_ALLOC_STATS=y (see include/alloc_strategy.h).
 * every strategy is an index of recycled blocks plus a cursor over never-used ones. from_index vs
 * from_cursor says whether the index earns its memory.
 */
struct bsfs_alloc_stats {
  __u64 allocs;         ///< blocks handed out
  __u64 frees;          ///< real 1->0 transitions only
  __u64 from_index;     ///< allocs served by the strategy's index
  __u64 from_cursor;    ///< allocs served by the sequential cursor
  __u64 fallback_scans; ///< full bitmap scans
  __u64 index_peak;     ///< largest the strategy's index got, in entries
};
#endif

/**
 * @brief Per-mount state, hung off sb->s_fs_info.
 *
 * Lock order, outermost first: a tree lock (inode_tree_lock, rec_lock or an
 * inode's truncate_mutex), then bitmap_lock or pending_free_lock. The allocator
 * takes those two itself and never holds both.
 */
struct bsfs_super_info {
  bsfs_super_block    *super;                ///< points into bh, so changing a field only needs mark_buffer_dirty(bh)
  struct buffer_head  *bh;                   ///< block 1, pinned for the whole mount
  __u8               **bitmap;               ///< bits of each bitmap chain block, see load_bitmap()
  struct buffer_head **bitmap_map;           ///< the buffer heads behind bitmap
  __u32                next_inode_number;    ///< scratch for get_next_inode_number(), recomputed every call
  struct mutex         rec_lock;             ///< serializes the recovery tree
  struct mutex         inode_tree_lock;      ///< serializes the inode tree
  struct mutex         bitmap_lock;          ///< guards the bitmap, the free counts and the strategy's index
  __u32                next_recovery_id;     ///< next id bsfs_rec_tree_add() hands out
  __u32                cached_recovery_root; ///< recovery_root next_recovery_id was computed from

  /// counted at mount, kept current by the allocator. under bitmap_lock.
  /// reserved_blocks keeps a full fs recoverable. under cow even freeing a span allocates first,
  /// so data allocs stop at the reserve and metadata allocs may spend it.
  __u32 free_blocks;
  __u32 reserved_blocks; ///< the metadata reserve, see bsfs_init_block_counts()

  /// owned by `src/alloc_<strategy>.c`, built at mount, never on disk. under bitmap_lock.
  void *alloc_private;

#ifdef BSFS_ALLOC_STATS
  struct bsfs_alloc_stats alloc_stats; ///< only with BSFS_ALLOC_STATS
#endif

  /// errno from the last ->write_inode that failed, so some inode root is memory-only. while set
  /// no deferred block may be released, we can't tell which ones that root replaced. -ENOSPC is routine.
  int writeback_error;

  /// cow-replaced blocks, released at a commit point once the shadow tree and its roots are on
  /// disk. see bsfs_commit_deferred_frees().
  __u32       *pending_free;
  size_t       pending_free_count; ///< entries in pending_free
  size_t       pending_free_cap;   ///< room in pending_free before it grows
  struct mutex pending_free_lock;  ///< guards pending_free and writeback_error

  struct bsfs_reclaim_stats *reclaim_stats; ///< NULL if they couldn't be allocated at mount
};

/** @brief The bsfs_super_info of a mounted superblock. */
static inline struct bsfs_super_info *bsfs_sb(struct super_block *sb) {
  return sb->s_fs_info;
}

/** @brief An inode tree entry, still pinned in its leaf. See bsfs_read_disk_inode(). */
struct disk_mapped_inode {
  struct disk_mapped_inode_t *inode; ///< points into bh->b_data
  struct buffer_head         *bh;    ///< the leaf, the caller brelse()s it
};

/** @brief A VFS inode plus what bsfs keeps per inode. Comes from the slab cache in inode.c. */
struct bsfs_inode {
  __u32 btree_root; ///< extent tree for files and symlinks, dentry tree for directories

  /// serializes bspan-tree ops on the inode (get_block vs truncate vs concurrent writers)
  struct mutex truncate_mutex;

  /// stashed by bsfs_unlink for the recovery snapshot evict builds at i_nlink == 0.
  /// non-zero parent ino means a snapshot is pending (valid inos are >= 1).
  __u32 i_deleted_parent_ino;
  char  i_deleted_name[MAX_FILENAME]; ///< name the snapshot records

  struct inode vfs_inode; ///< the embedded VFS inode, see bsfs_i()
};

/** @brief The bsfs_inode around a VFS inode. */
static inline struct bsfs_inode *bsfs_i(struct inode *inode) {
  return container_of(inode, struct bsfs_inode, vfs_inode);
}

/** @brief Which of the four kinds of tree a handle reads. */
typedef enum {
  BTREE_TYPE_INODE,    ///< the inode tree, disk_mapped_inode_t
  BTREE_TYPE_BSPAN,    ///< a file's extents, bspan_t
  BTREE_TYPE_RECOVERY, ///< the recovery tree, rec_entry_t
  BTREE_TYPE_DIR_ENTRY ///< a directory, bsfs_dentry
} btree_data_type_t;

/**
 * how far an allocation may dig into the metadata reserve.
 *
 * under cow, freeing space allocates first, so if growth can eat the last block the fs can never be
 * emptied again. the reserve is there for the shrinking paths.
 *
 * @code{.unparsed}
 *   GROW      creates, links, file extension. stops at the reserve
 *   WRITEBACK ->write_inode. may take half, persisting a root is what frees deferred blocks
 *   SHRINK    truncate, unlink, evict, recovery purge. may take all of it
 * @endcode
 */
typedef enum { BSFS_ALLOC_GROW = 0, BSFS_ALLOC_WRITEBACK, BSFS_ALLOC_SHRINK } bsfs_alloc_prio_t;

/** @brief Orders two whole entries, negative, zero or positive like strcmp(). */
typedef int (*btree_compare_fn)(const void *a, const void *b);

/** @brief Extracts the numeric key of an entry. */
typedef __u32 (*btree_key_fn)(const void *entry);

/**
 * @brief Says which tree to work on and where its root is.
 *
 * Make one with btree_create_handle() or a typed wrapper from btree.h.
 */
typedef struct {
  btree_data_type_t type;       ///< which kind of tree
  __u64             entry_size; ///< bytes per entry
  btree_compare_fn  cmp;        ///< orders whole entries
  btree_key_fn      get_key;    ///< key btree_search() and btree_remove() go by
  __u32             root_block; ///< current root, 0 for an empty tree. every mutation moves it
  __u8              min_degree; ///< t, from bsfs_min_degree()

  /// bsfs_alloc_prio_t for this handle's mutations
  __u8 prio;
} btree_handle_t;

/** @brief A tree node in memory. Release it with btree_free_node(). */
struct btree_node {
  struct btree_node_t *node; ///< points into bh->b_data
  struct buffer_head  *bh;   ///< pins the block
};

#endif
