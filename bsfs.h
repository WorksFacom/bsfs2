#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include "include/bsfs_shared.h"
#include <linux/atomic.h>
#include <linux/buffer_head.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define BSFS_V1 0x0001
#define BSFS_V2 0x0002

#define BSFS_VALID_FS 0x0001
#define BSFS_ERROR_FS 0x0002

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
 * What the allocation strategy actually did, for the tests and the benchmarks.
 *
 * Compiled in only with BSFS_ALLOC_STATS=y - see include/alloc_strategy.h. The
 * index/cursor split is the interesting one: every strategy so far is some
 * structure of recycled blocks plus a cursor over the never-yet-handed-out ones,
 * and the ratio between them is what says whether the structure is earning its
 * memory.
 */
struct bsfs_alloc_stats {
  __u64 allocs;         // blocks handed out
  __u64 frees;          // blocks returned (real 1->0 transitions only)
  __u64 from_index;     // ... of the allocs, how many came from the strategy's structure
  __u64 from_cursor;    // ... and how many from the sequential cursor
  __u64 fallback_scans; // times the strategy had to fall back to a full scan
  __u64 index_peak;     // most entries the structure ever held at once
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

  // Counted once at mount and maintained by the allocator, so statfs and the
  // reserve check don't have to rescan the whole bitmap. Both under bitmap_lock.
  //
  // reserved_blocks is what keeps a full filesystem recoverable: under
  // copy-on-write even *freeing* a span has to allocate first, so data
  // allocations stop at the reserve while metadata ones may spend it.
  __u32 free_blocks;
  __u32 reserved_blocks;

  // Whatever index the compiled-in allocation strategy keeps over the bitmap.
  // Owned entirely by src/alloc_<strategy>.c, built at mount, dropped at
  // unmount, never written to disk. Also under bitmap_lock.
  void *alloc_private;

#ifdef BSFS_ALLOC_STATS
  struct bsfs_alloc_stats alloc_stats;
#endif

  // Set when ->write_inode could not persist an inode, which means some
  // per-inode root is still only in memory. While it is set no deferred block
  // may be released: we cannot tell which ones the failed root replaced.
  bool writeback_failed;

  // Blocks that copy-on-write replaced. They are only handed back to the
  // allocator at a commit point, once the shadow tree and the roots pointing at
  // it are on disk - see bsfs_commit_deferred_frees().
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

  // stashed by bsfs_unlink so bsfs_evict_inode can build the recovery snapshot once the inode is truly gone (i_nlink == 0)
  // a non-zero parent ino doubles as a "snapshot pending" flag (valid inos are >= 1)
  __u32 i_deleted_parent_ino;
  char  i_deleted_name[MAX_FILENAME];

  struct inode vfs_inode;
};

static inline struct bsfs_inode *bsfs_i(struct inode *inode) {
  return container_of(inode, struct bsfs_inode, vfs_inode);
}

typedef enum { BTREE_TYPE_INODE, BTREE_TYPE_BSPAN, BTREE_TYPE_RECOVERY, BTREE_TYPE_DIR_ENTRY } btree_data_type_t;

/*
 * How far an allocation may eat into the metadata reserve.
 *
 * Copy-on-write inverts the usual order of things: releasing space requires
 * allocating first, so a filesystem that lets ordinary growth consume the last
 * block can never be emptied again. The reserve exists to keep the shrinking
 * paths working, so they are the ones that outrank everybody.
 *
 *   GROW      creates, links, extending a file - stops at the whole reserve
 *   WRITEBACK ->write_inode - may take half of it, since persisting a root is
 *             what makes deferred blocks releasable
 *   SHRINK    truncate, unlink, evict, recovery purge - may take all of it
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

  // How deep into the reserve this handle's mutations may dig. See
  // bsfs_alloc_prio_t: the whole point is that shrinking the tree outranks
  // growing it, because under copy-on-write shrinking is also an allocation.
  __u8 prio;
} btree_handle_t;

struct btree_node {
  struct btree_node_t *node;
  struct buffer_head  *bh;
};

#endif
