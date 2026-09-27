#include "bsfs.h"
#include "include/alloc_strategy.h"
#include "include/bitmap.h"
#include "include/btree.h"
#include "include/dir.h"
#include "include/file.h"
#include "include/inode.h"
#include "include/rec_entry.h"
#include "include/reclaim_stats.h"
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/highuid.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vfs.h>
#include <linux/writeback.h>

static void bsfs_put_super(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  bsfs_reclaim_stats_unmount(sb);
  // sbi->super points inside sbi->bh; there is nothing to free separately.
  // The commit does the writing, in the right order - dirtying the buffer and
  // syncing it here first would push the roots out ahead of their trees.
  mark_buffer_dirty(sbi->bh);
  (void)bsfs_commit_deferred_frees(sb);

  bsfs_alloc_log_unmount(sb);
  bsfs_alloc_strategy_destroy(sb);

  release_bitmap(sb);
  brelse(sbi->bh);
  kfree(sbi);

  sb->s_fs_info = NULL;
}

/*
 * The superblock buffer carries the tree roots (inode_root, recovery_root), so
 * a sync(2) has to push it out; without this it only reached the disk at
 * unmount time.
 */
static int bsfs_sync_fs(struct super_block *sb, int wait) {
  if (!wait)
    return 0;

  // A commit point: the VFS has already written the inodes back, so the barrier
  // inside the commit covers every root and the blocks copy-on-write replaced
  // can finally be reused. It writes sbi->bh itself, after the trees.
  return bsfs_commit_deferred_frees(sb);
}

/*
 * Without this, df(1) and statfs(2) report nothing for a bsfs mount, which also
 * makes it awkward to test running out of space. Blocks still parked as
 * deferred frees are counted as used, since that is what they are until the
 * next commit point.
 *
 * f_bavail excludes the metadata reserve: that is space unprivileged writes
 * genuinely cannot have.
 */
static int bsfs_statfs(struct dentry *dentry, struct kstatfs *buf) {
  struct super_block     *sb  = dentry->d_sb;
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   free_blocks, avail;

  mutex_lock(&sbi->bitmap_lock);
  free_blocks = sbi->free_blocks;
  avail       = (free_blocks > sbi->reserved_blocks) ? free_blocks - sbi->reserved_blocks : 0;
  mutex_unlock(&sbi->bitmap_lock);

  buf->f_type    = MAGIC_NUMBER;
  buf->f_bsize   = sb->s_blocksize;
  buf->f_blocks  = sbi->super->total_blocks - sbi->super->data_block_start;
  buf->f_bfree   = free_blocks;
  buf->f_bavail  = avail;
  buf->f_namelen = MAX_FILENAME - 1;

  return 0;
}

static const struct super_operations bsfs_sops = {
    .alloc_inode = bsfs_alloc_inode,
    .write_inode = bsfs_write_inode,
    .free_inode  = bsfs_free_cache_inode,
    .evict_inode = bsfs_evict_inode,
    .put_super   = bsfs_put_super,
    .sync_fs     = bsfs_sync_fs,
    .statfs      = bsfs_statfs,
};

static int bsfs_fill_super(struct super_block *s, struct fs_context *fc) {
  int ret    = -EINVAL;
  int silent = fc->sb_flags & SB_SILENT;

  struct buffer_head      *bh;
  struct bsfs_super_block *bsfs_super;

  struct inode           *root_inode;
  struct bsfs_super_info *sbi;

  sbi = kzalloc(sizeof(struct bsfs_super_info), GFP_KERNEL);

  if (!sbi)
    return -ENOMEM;

  s->s_fs_info = sbi;

  mutex_init(&sbi->inode_tree_lock);
  mutex_init(&sbi->bitmap_lock);
  mutex_init(&sbi->pending_free_lock);

  if (!sb_set_blocksize(s, BSFS_BLOCK_SIZE)) {
    pr_err("unable to set block size");
    goto out_bad_hblock;
  }

  if (!(bh = sb_bread(s, 1))) {
    pr_err("unable to read super block");
    goto out_bad_sb;
  }

  // sbi->super points DIRECTLY at the superblock buffer: whoever changes a
  // field (e.g. inode_root after a split) only needs mark_buffer_dirty(sbi->bh).
  // With a kzalloc'd copy those changes never reached the disk.
  bsfs_super = (struct bsfs_super_block *)bh->b_data;
  sbi->super = bsfs_super;
  sbi->bh    = bh;
  s->s_magic = bsfs_super->magic_number;

  // check exact data location - magic_number is first member
  if (*(__u32 *)(bh->b_data) != MAGIC_NUMBER) {
    pr_err("magic number doesn't match");
    goto out_no_fs;
  }

  // Allocate bitmap to keep the superblock small
  ret = load_bitmap(s);

  if (ret)
    goto out_freemap;

  bsfs_init_block_counts(s);

  ret = bsfs_alloc_strategy_init(s);

  if (ret)
    goto out_freemap;

  bsfs_alloc_log_mount(s);

  bsfs_rec_tree_mount_init(s);

  /* set up enough so that it can read an inode */
  s->s_op       = &bsfs_sops;
  s->s_time_min = 0;
  s->s_time_max = U32_MAX;
  root_inode    = bsfs_get_vfs_inode(s, BSFS_ROOT_INO);

  if (IS_ERR(root_inode)) {
    ret = PTR_ERR(root_inode);
    goto out_no_root;
  }

  ret = -ENOMEM;

  s->s_root = d_make_root(root_inode);
  if (!s->s_root)
    goto out_no_root;

  bsfs_reclaim_stats_mount(s);
  return 0;

out_no_root:
  if (!silent)
    printk("BSFS: get root inode failed\n");

out_freemap:
  // load_bitmap() pins one buffer head per bitmap block and can fail halfway
  // through; release_bitmap() copes with a partial load. Without this every
  // failed mount leaked all of them.
  bsfs_alloc_strategy_destroy(s);
  release_bitmap(s);
  goto out_release;

out_no_fs:
  ret = -EINVAL;
  if (!silent)
    printk("VFS: Can't find a BSFS filesystem on device %s.\n", s->s_id);

out_release:
  brelse(bh);
  goto out;

out_bad_hblock:
  printk("BSFS: blocksize too small for device\n");
  goto out;

out_bad_sb:
  printk("BSFS: unable to read superblock\n");

out:
  s->s_fs_info = NULL;
  kfree(sbi);
  return ret;
}

static int bsfs_get_tree(struct fs_context *fc) {
  return get_tree_bdev(fc, bsfs_fill_super);
}

static const struct fs_context_operations bsfs_context_ops = {
    .get_tree = bsfs_get_tree,
    // TODO
    // .reconfigure = bsfs_reconfigure
};

static int bsfs_init_fs_context(struct fs_context *fc) {
  fc->ops = &bsfs_context_ops;

  return 0;
}

static struct file_system_type bsfs_fs_type = {
    .owner           = THIS_MODULE,
    .name            = "bsfs",
    .fs_flags        = FS_REQUIRES_DEV,
    .init_fs_context = bsfs_init_fs_context,
    .kill_sb         = kill_block_super,
};

MODULE_ALIAS_FS("bsfs");

static int __init init_bsfs(void) {
  int err = init_inodecache();

  if (err)
    goto out1;

  err = bsfs_reclaim_debugfs_init();

  if (err)
    goto out;

  err = register_filesystem(&bsfs_fs_type);

  if (err)
    goto out_debugfs;

  return 0;

out_debugfs:
  bsfs_reclaim_debugfs_exit();
out:
  destroy_inodecache();
out1:
  return err;
}

static void __exit exit_bsfs(void) {
  unregister_filesystem(&bsfs_fs_type);
  bsfs_reclaim_debugfs_exit();
  destroy_inodecache();
}

module_init(init_bsfs);
module_exit(exit_bsfs);
MODULE_DESCRIPTION("BSFS Filesystem");
MODULE_LICENSE("GPL");
