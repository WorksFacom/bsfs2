#include "bsfs.h"
#include "include/bitmap.h"
#include "include/btree.h"
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

static struct kmem_cache *bsfs_inode_cache;

static inline struct inode_info *bsfs_i(struct inode *inode) {
  return container_of(inode, struct inode_info, vfs_inode);
}

static struct inode *bsfs_alloc_inode(struct super_block *sb) {
  struct inode_info *bsfs_i;
  bsfs_i = alloc_inode_sb(sb, bsfs_inode_cache, GFP_KERNEL);
  if (!bsfs_i)
    return NULL;

  return &bsfs_i->vfs_inode;
}

// static int bsfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
//   int                 err = 0;
//   struct buffer_head *bh;
//
//   bh = v2_update_inode(inode);
//   if (!bh)
//     return -EIO;
//   if (wbc->sync_mode == WB_SYNC_ALL && buffer_dirty(bh)) {
//     sync_dirty_buffer(bh);
//     if (buffer_req(bh) && !buffer_uptodate(bh)) {
//       printk("IO error syncing inode [%s:%08lx]\n", inode->i_sb->s_id, inode->i_ino);
//       err = -EIO;
//     }
//   }
//   brelse(bh);
//   return err;
// }
//
// static void bsfs_free_inode(struct inode *inode) {
//   kmem_cache_free(bsfs_inode_cache, bsfs_i(inode));
// }
//
// static void bsfs_evict_inode(struct inode *inode) {
//   truncate_inode_pages_final(&inode->i_data);
//   if (!inode->i_nlink) {
//     inode->i_size = 0;
//     bsfs_truncate(inode);
//   }
//   invalidate_inode_buffers(inode);
//   clear_inode(inode);
//   if (!inode->i_nlink)
//     bsfs_free_inode(inode);
// }

// old read_inode() on btree.c
struct bsfs_inode *__read_disk_inode(struct super_block *sb, ino_t ino) {
  int ret;

  struct bsfs_super_info *sbi   = sb->s_fs_info;
  struct bsfs_inode      *inode = kzalloc(sizeof(bsfs_inode), GFP_KERNEL);

  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block     = sbi->super->inode_root;

  ret = btree_search(sb, inode_h, inode_h->root_block, ino, inode);

  // TODO: applicable?
  // if (!ino || ino > sbi->s_ninodes) {
  //   printk("Bad inode number on dev %s: %ld is out of range\n", sb->s_id, (long)ino);
  //   return NULL;
  // }

  return inode;
}

void bsfs_set_inode(struct inode *inode, dev_t rdev) {
  if (S_ISREG(inode->i_mode)) {
    inode->i_op             = &bsfs_file_inode_operations;
    inode->i_fop            = &bsfs_file_operations;
    inode->i_mapping->a_ops = &bsfs_aops;
  } else if (S_ISDIR(inode->i_mode)) {
    inode->i_op             = &bsfs_dir_inode_operations;
    inode->i_fop            = &bsfs_dir_operations;
    inode->i_mapping->a_ops = &bsfs_aops;
  } else if (S_ISLNK(inode->i_mode)) {
    inode->i_op = &bsfs_symlink_inode_operations;
    inode_nohighmem(inode);
    inode->i_mapping->a_ops = &bsfs_aops;
  } else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) || S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
    init_special_inode(inode, inode->i_mode, rdev);
  } else {
    printk(KERN_DEBUG "BSFS: Invalid file type 0%04o for inode %lu.\n", inode->i_mode, inode->i_ino);
    make_bad_inode(inode);
  }
}

static struct inode *__set_up_inode_new(struct inode *inode) {
  int                i;
  struct bsfs_inode *disk_inode;
  struct inode_info *bsfs_inode = bsfs_i(inode);

  // TODO: handle error
  disk_inode = __read_disk_inode(inode->i_sb, inode->i_ino);
  if (!disk_inode) {
    iget_failed(inode);
    return ERR_PTR(-EIO);
  }

  if (disk_inode->nlinks == 0) {
    printk("BSFS: deleted inode referenced: %lu\n", inode->i_ino);
    kfree(disk_inode);
    iget_failed(inode);
    return ERR_PTR(-ESTALE);
  }

  inode->i_mode = disk_inode->mode;
  i_uid_write(inode, disk_inode->uid);
  i_gid_write(inode, disk_inode->gid);
  set_nlink(inode, disk_inode->nlinks);

  // TODO: suggestion: In kernel code, prefer helpers like i_size_read(), i_size_write(), or truncate_setsize() when changing/reading after
  // inode is live (locking/atomicity). Applicable?
  inode->i_size = disk_inode->size;

  inode_set_mtime(inode, disk_inode->mtime, 0);
  inode_set_atime(inode, disk_inode->atime, 0);
  inode_set_ctime(inode, disk_inode->ctime, 0);

  // TODO: whats this?
  inode->i_blocks = 0;

  for (i = 0; i < 10; i++)
    bsfs_inode->u.i2_data[i] = disk_inode->i_zone[i];

  // TODO: support special file inodes
  // bsfs_set_inode(inode, old_decode_dev(rdev));
  bsfs_set_inode(inode, 0);
  kfree(disk_inode);
  unlock_new_inode(inode);
  return inode;
}

struct inode *bsfs_get_vfs_inode(struct super_block *sb, unsigned long ino) {
  struct inode *inode;

  inode = iget_locked(sb, ino);

  if (!inode)
    return ERR_PTR(-ENOMEM);

  if (!(inode->i_state & I_NEW))
    return inode;

  return __set_up_inode_new(inode);
}

static const struct super_operations bsfs_sops = {
    .alloc_inode = bsfs_alloc_inode,
    // .write_inode = bsfs_write_inode,
    // .free_inode  = bsfs_free_inode,
    // .evict_inode = bsfs_evict_inode,
    // .put_super	= bsfs_put_super,
    // .statfs		= bsfs_statfs,
};

static int bsfs_fill_super(struct super_block *s, struct fs_context *fc) {
  unsigned long i, block;

  int ret    = -EINVAL;
  int silent = fc->sb_flags & SB_SILENT;

  struct buffer_head      *bh;
  struct buffer_head     **map;
  struct bsfs_super_block *bsfs_super;

  struct inode           *root_inode;
  struct bsfs_super_info *sbi;

  sbi = kzalloc(sizeof(struct bsfs_super_info), GFP_KERNEL);
  if (!sbi)
    return -ENOMEM;
  s->s_fs_info = sbi;

  sbi->super = kzalloc(sizeof(struct bsfs_super_block), GFP_KERNEL);
  if (!sbi->super)
    return -ENOMEM;

  // TODO
  // BUILD_BUG_ON(64 != sizeof(struct bsfs_inode));

  if (!sb_set_blocksize(s, BLOCK_SIZE))
    goto out_bad_hblock;

  if (!(bh = sb_bread(s, 1)))
    goto out_bad_sb;

  bsfs_super = (struct bsfs_super_block *)bh->b_data;
  sbi->bh    = bh;
  s->s_magic = bsfs_super->magic_number;

  // check exact data location - magic_number is first member
  if (*(__u32 *)(bh->b_data) == MAGIC_NUMBER) {
    goto out_no_fs;
  }

  // Allocate bitmap to keep the superblock small
  load_bitmap(s);

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

  ret       = -ENOMEM;
  s->s_root = d_make_root(root_inode);
  if (!s->s_root)
    goto out_no_root;

  if (!(sbi->s_mount_state & BSFS_VALID_FS))
    printk("BSFS: mounting unchecked file system, "
           "running fsck is recommended\n");
  else if (sbi->s_mount_state & BSFS_ERROR_FS)
    printk("BSFS: mounting file system with errors, "
           "running fsck is recommended\n");

  return 0;

out_no_root:
  if (!silent)
    printk("BSFS: get root inode failed\n");
  goto out_freemap;

out_no_bitmap:
  printk("BSFS: bad superblock or unable to read bitmaps\n");

out_freemap:
  // TODO
  //   for (i = 0; i < sbi->s_imap_blocks; i++)
  //     brelse(sbi->s_imap[i]);
  //   for (i = 0; i < sbi->s_zmap_blocks; i++)
  //     brelse(sbi->s_zmap[i]);
  //   kfree(sbi->s_imap);
  //   goto out_release;

out_no_map:
  ret = -ENOMEM;
  if (!silent)
    printk("BSFS: can't allocate map\n");
  goto out_release;

out_illegal_sb:
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
    // TODO: implement kill_sb
    // .kill_sb	      	= kill_block_super,
};

MODULE_ALIAS_FS("bsfs");

static void init_once(void *foo) {
  struct bsfs_inode *bsfs_inode = (struct bsfs_inode *)foo;

  inode_init_once(&bsfs_inode->vfs_inode);
}

static int __init init_inodecache(void) {
  bsfs_inode_cache = kmem_cache_create("bsfs_inode_cache", sizeof(struct bsfs_inode), 0, (SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT), init_once);
  if (bsfs_inode_cache == NULL)
    return -ENOMEM;
  return 0;
}

static void destroy_inodecache(void) {
  /*
   * Make sure all delayed rcu free inodes are flushed before we
   * destroy cache.
   */
  rcu_barrier();
  kmem_cache_destroy(bsfs_inode_cache);
}

static int __init init_bsfs(void) {
  // TODO: init cache

  int err = register_filesystem(&bsfs_fs_type);
  if (err)
    return err;

  return 0;
}

static void __exit exit_bsfs(void) {
  unregister_filesystem(&bsfs_fs_type);
  // TODO: destroy cache
}

module_init(init_bsfs);
module_exit(exit_bsfs);
MODULE_DESCRIPTION("BSFS Filesystem");
MODULE_LICENSE("AGPL");
