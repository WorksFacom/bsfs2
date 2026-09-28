#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include "include/bsfs_shared.h"
#include <linux/atomic.h>
#include <linux/buffer_head.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define DEFAULT_RECOVERY_TTL (30ULL * 24ULL * 60ULL * 60ULL)

#define BSFS_RECLAIM_ERROR_SLOTS 16

struct bsfs_reclaim_error {
  int   error;
  __u64 count;
};

struct bsfs_reclaim_stats {
  struct kref references;
  atomic64_t attempts;
  atomic64_t successes;
  atomic64_t entries_purged;
  atomic64_t blocks_reclaimed;
  atomic64_t pre_commit_ns;
  atomic64_t purge_ns;
  atomic64_t post_commit_ns;
  atomic64_t failures;
  spinlock_t error_lock;
  struct bsfs_reclaim_error errors[BSFS_RECLAIM_ERROR_SLOTS];
  struct dentry *debugfs_dir;
};

#ifdef BSFS_ALLOC_STATS
/*
 * allocator counters for tests and benchmarks, only with BSFS_ALLOC_STATS=y (see include/alloc_strategy.h).
 * every strategy is an index of recycled blocks plus a cursor over never-used ones. from_index vs
 * from_cursor says whether the index earns its memory.
 */
struct bsfs_alloc_stats {
  __u64 allocs;
  __u64 frees;          // real 1->0 transitions only
  __u64 from_index;     // allocs served by the strategy's index
  __u64 from_cursor;    // allocs served by the sequential cursor
  __u64 fallback_scans; // full bitmap scans
  __u64 index_peak;
};
#endif

struct bsfs_super_info {
  bsfs_super_block    *super;
  struct buffer_head  *bh;
  __u8               **bitmap;
  struct buffer_head **bitmap_map;
  __u32                next_inode_number;
  struct mutex         rec_lock;
  struct mutex inode_tree_lock;
  struct mutex bitmap_lock;
  __u32        next_recovery_id;
  __u32        cached_recovery_root;

  // counted at mount, kept current by the allocator. under bitmap_lock.
  // reserved_blocks keeps a full fs recoverable. under cow even freeing a span allocates first,
  // so data allocs stop at the reserve and metadata allocs may spend it.
  __u32 free_blocks;
  __u32 reserved_blocks;

  // owned by src/alloc_<strategy>.c, built at mount, never on disk. under bitmap_lock.
  void *alloc_private;

#ifdef BSFS_ALLOC_STATS
  struct bsfs_alloc_stats alloc_stats;
#endif

  // errno from the last ->write_inode that failed, so some inode root is memory-only. while set
  // no deferred block may be released, we can't tell which ones that root replaced. -ENOSPC is routine.
  int writeback_error;

  // cow-replaced blocks, released at a commit point once the shadow tree and its roots are on
  // disk. see bsfs_commit_deferred_frees().
  __u32       *pending_free;
  size_t       pending_free_count;
  size_t       pending_free_cap;
  struct mutex pending_free_lock;

  struct bsfs_reclaim_stats *reclaim_stats;
};

static inline struct bsfs_super_info *bsfs_sb(struct super_block *sb) {
  return sb->s_fs_info;
}

struct disk_mapped_inode {
  struct disk_mapped_inode_t *inode;
  struct buffer_head         *bh;
};

struct bsfs_inode {
  __u32 btree_root;

  // serializes bspan-tree ops on the inode (get_block vs truncate vs concurrent writers)
  struct mutex truncate_mutex;

  // stashed by bsfs_unlink for the recovery snapshot evict builds at i_nlink == 0.
  // non-zero parent ino means a snapshot is pending (valid inos are >= 1).
  __u32 i_deleted_parent_ino;
  char  i_deleted_name[MAX_FILENAME];

  struct inode vfs_inode;
};

static inline struct bsfs_inode *bsfs_i(struct inode *inode) {
  return container_of(inode, struct bsfs_inode, vfs_inode);
}

typedef enum { BTREE_TYPE_INODE, BTREE_TYPE_BSPAN, BTREE_TYPE_RECOVERY, BTREE_TYPE_DIR_ENTRY } btree_data_type_t;

/*
 * how far an allocation may dig into the metadata reserve.
 *
 * under cow, freeing space allocates first, so if growth can eat the last block the fs can never be
 * emptied again. the reserve is there for the shrinking paths.
 *
 *   GROW      creates, links, file extension. stops at the reserve
 *   WRITEBACK ->write_inode. may take half, persisting a root is what frees deferred blocks
 *   SHRINK    truncate, unlink, evict, recovery purge. may take all of it
 */
typedef enum { BSFS_ALLOC_GROW = 0, BSFS_ALLOC_WRITEBACK, BSFS_ALLOC_SHRINK } bsfs_alloc_prio_t;

typedef int (*btree_compare_fn)(const void *a, const void *b);

typedef __u32 (*btree_key_fn)(const void *entry);

typedef struct {
  btree_data_type_t type;
  __u64             entry_size;
  btree_compare_fn  cmp;
  btree_key_fn      get_key;
  __u32             root_block;
  __u8              min_degree;

  // bsfs_alloc_prio_t for this handle's mutations
  __u8 prio;
} btree_handle_t;

struct btree_node {
  struct btree_node_t *node;
  struct buffer_head  *bh;
};

#endif
