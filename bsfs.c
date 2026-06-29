#include "bsfs.h"
#include "include/bitmap.h"
#include "include/btree.h"
#include "include/dir.h"
#include "include/file.h"
#include "include/inode.h"
#include "include/rec_entry.h"
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

  brelse(sbi->bh);
  kfree(sbi->super);
  kfree(sbi);

  sb->s_fs_info = NULL;
}

static const struct super_operations bsfs_sops = {
    .alloc_inode = bsfs_alloc_inode,
    .write_inode = bsfs_write_inode,
    .free_inode  = bsfs_free_cache_inode,
    .evict_inode = bsfs_evict_inode,
    .put_super   = bsfs_put_super,
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

  sbi->super = kzalloc(sizeof(struct bsfs_super_block), GFP_KERNEL);

  if (!sbi->super) {
    s->s_fs_info = NULL;
    kfree(sbi);
    return -ENOMEM;
  }

  // if the struct size ever changes, the kernel won't compile
  // catches accidental ABI breaks or padding changes at build time.
  BUILD_BUG_ON(856 != sizeof(struct bsfs_inode));

  if (!sb_set_blocksize(s, BSFS_BLOCK_SIZE)) {
    pr_err("unable to set block size");
    goto out_bad_hblock;
  }

  if (!(bh = sb_bread(s, 1))) {
    pr_err("unable to read super block");
    goto out_bad_sb;
  }

  bsfs_super = (struct bsfs_super_block *)bh->b_data;
  memcpy(sbi->super, bh->b_data, sizeof(struct bsfs_super_block));
  sbi->bh    = bh;
  s->s_magic = bsfs_super->magic_number;

  // check exact data location - magic_number is first member
  if (*(__u32 *)(bh->b_data) != MAGIC_NUMBER) {
    pr_err("magic number doesn't match");
    goto out_no_fs;
  }

  // Allocate bitmap to keep the superblock small
  load_bitmap(s);

  bsfs_rec_tree_mount_init(s);

  // TODO
  // __set_bit(0, sbi->s_imap[0]->b_data);

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

  return 0;

out_no_root:
  if (!silent)
    printk("BSFS: get root inode failed\n");
  goto out_freemap;

  // TODO: handle this goto
  // out_no_bitmap:
  printk("BSFS: bad superblock or unable to read bitmaps\n");

// TODO
out_freemap:
  //   for (i = 0; i < sbi->s_imap_blocks; i++)
  //     brelse(sbi->s_imap[i]);
  //   for (i = 0; i < sbi->s_zmap_blocks; i++)
  //     brelse(sbi->s_zmap[i]);
  //   kfree(sbi->s_imap);
  //   goto out_release;

  // TODO: handle this goto
  // out_no_map:
  ret = -ENOMEM;
  if (!silent)
    printk("BSFS: can't allocate map\n");
  goto out_release;

  // TODO: handle this goto
  // out_illegal_sb:
  if (!silent)
    printk("BSFS: bad superblock\n");
  goto out_release;

out_no_fs:
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
  kfree(sbi->super);
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

  err = register_filesystem(&bsfs_fs_type);

  if (err)
    goto out;

  return 0;

out:
  destroy_inodecache();
out1:
  return err;
}

static void __exit exit_bsfs(void) {
  unregister_filesystem(&bsfs_fs_type);
  // TODO: destroy cache
}

module_init(init_bsfs);
module_exit(exit_bsfs);
MODULE_DESCRIPTION("BSFS Filesystem");
// MODULE_LICENSE("AGPL");
MODULE_LICENSE("GPL");
