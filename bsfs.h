#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include <linux/buffer_head.h>
#include <linux/types.h>

#define BSFS_V1 0x0001
#define BSFS_V2 0x0002

#define BSFS_VALID_FS 0x0001
#define BSFS_ERROR_FS 0x0002

#define MAGIC_NUMBER 0x53465342

#define BSFS_ROOT_INO 1

#define BSFS_BLOCK_SIZE 4096

#define MAX_FILENAME 255

/** Usable bytes in a bitmap block (reserve 4B for next pointer). */
#define BITMAP_BLOCK_BYTES (BLOCK_SIZE - sizeof(__u32))

#define BITS_PER_BLOCK (BITMAP_BLOCK_BYTES * 8)

/** Sentinel for an invalid or absent block address. */
#define INVALID_BLOCK 0xFFFFFFFF

/** Default retention period for recovery entries (seconds). */
#define DEFAULT_RECOVERY_TTL (30ULL * 24ULL * 60ULL * 60ULL)

// on-disk super block
typedef struct bsfs_super_block {
  __u32 magic_number;
  __u32 block_size;
  __u64 fs_size; // Total size (in bytes) of the file system image or device. 64-bit to support 4+ GiB.
  __u32 total_blocks;
  __u32 block_bitmap_start; // Block number where the bitmap structure begins.
  __u32 block_bitmap_total;
  __u32 inode_root;    // Block number of the root node of the B-tree that stores inode entries.
  __u32 recovery_root; // Block number of the root node of the B-tree used for recovery records.
  __u32 data_block_start;
  __u32 root_inode; // The inode number of the root directory of the file system.
} bsfs_super_block;

// in-memory super block
struct bsfs_super_info {
  bsfs_super_block    *super;
  struct buffer_head  *bh;
  __u8               **bitmap;
  struct buffer_head **bitmap_map;
};

static inline struct bsfs_super_info *bsfs_sb(struct super_block *sb) {
  return sb->s_fs_info;
}

typedef struct {
  __u8 bits[BITMAP_BLOCK_BYTES]; // Byte array representing allocation status of a range of blocks. 1 bit = 1 block - 0 = free, 1 =
                                 // allocated.
  __u32 next_block;              // Block number of the next bitmap block in the linked list. If last block, value is INVALID_BLOCK.
} bitmap_block_t;

typedef struct {
  __u32 file_blk; // logical block
  __u32 disk_blk; // physical block number where span begins
  __u16 length;
  __u16 padding; // unused
} bspan_t;

struct disk_mapped_inode_t {
  __u32 ino;
  __u32 size;
  __u8  unused;      // file_type
  __u32 block_count; // TODO: is this being updated?
  __u16 uid;
  __u16 gid;
  __u16 mode; // was permissions
  __u16 nlinks;
  __u32 atime;
  __u32 mtime;
  __u32 ctime;
  __u32 direct_blocks[48]; // 16 * bspan_t
  __u32 btree_root;
  __u8  pad[34];
};

struct disk_mapped_inode {
  struct disk_mapped_inode_t *inode;
  struct buffer_head         *bh;
};

struct bsfs_inode {
  // bspan_t      direct_blocks[16];
  __u32 btree_root;
  // add fields as needed;

  struct inode vfs_inode;
};

static inline struct bsfs_inode *bsfs_i(struct inode *inode) {
  return container_of(inode, struct bsfs_inode, vfs_inode);
}

typedef struct {
  __u32   recovery_id;
  __u64   deleted_at;
  __u32   original_parent_ino;
  char    original_name[256];
  __u32   inode_number;
  __u8    file_type;
  __u16   uid;
  __u16   gid;
  __u16   permissions;
  __u32   file_size;
  __u32   block_count;
  bspan_t direct_blocks[16];
  __u32   btree_root;
  __u64   retention_until;
} rec_entry_t;

struct bsfs_dentry {
  __u32 ino;
  __u32 name_hash; // Hash value of the filename, used for indexing or fast lookup.
  char  filename[MAX_FILENAME];
};

typedef enum { BTREE_TYPE_INODE, BTREE_TYPE_BSPAN, BTREE_TYPE_RECOVERY, BTREE_TYPE_DIR_ENTRY } btree_data_type_t;

typedef int (*btree_compare_fn)(const void *a, const void *b);

typedef __u32 (*btree_key_fn)(const void *entry);

typedef struct {
  btree_data_type_t type;
  __u64             entry_size;
  btree_compare_fn  cmp;
  btree_key_fn      get_key;
  __u32             root_block; // block number of the root node on disk.
  __u8              min_degree;
} btree_handle_t;

struct btree_node_t {
  __u32 num_entries;
  __u8  is_leaf;
  __u8  data[4091]; // remaining bytes (entries + children)
};

struct btree_node {
  struct btree_node_t *node; // TODO: remove. Use helper instead: btree_bh_get_node(buffer_head *bh) { return (btree_node_t *)bh->b_data; }
  struct buffer_head  *bh;
};

int bsfs_get_block(struct inode *inode, sector_t block, struct buffer_head *bh, int create);

#endif // FILESYSTEM_BSFS
