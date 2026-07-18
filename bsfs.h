#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include "include/bsfs_shared.h"
#include <linux/buffer_head.h>
#include <linux/types.h>

#define BSFS_V1 0x0001
#define BSFS_V2 0x0002

#define BSFS_VALID_FS 0x0001
#define BSFS_ERROR_FS 0x0002

#define DEFAULT_RECOVERY_TTL (30ULL * 24ULL * 60ULL * 60ULL)

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

typedef int (*btree_compare_fn)(const void *a, const void *b);

typedef __u32 (*btree_key_fn)(const void *entry);

typedef struct {
  btree_data_type_t type;
  __u64             entry_size;
  btree_compare_fn  cmp;
  btree_key_fn      get_key;
  __u32             root_block;
  __u8              min_degree;
} btree_handle_t;

struct btree_node {
  struct btree_node_t *node;
  struct buffer_head  *bh;
};

#endif
