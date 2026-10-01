#include "../include/inode.h"
#include "../include/btree.h"
#include "../include/dir.h"
#include "../include/file.h"
#include "../include/rec_entry.h"
#include <linux/mpage.h>
#include <linux/writeback.h>

static struct kmem_cache *bsfs_inode_cache;

static int inode_tree_find_max(struct super_block *sb, btree_handle_t *handle, __u32 node_block, struct disk_mapped_inode **out) {
  int                ret;
  struct btree_node *node = NULL;

  *out = NULL;

  if (!handle || node_block == 0 || node_block == INVALID_BLOCK)
    return 0;

  while (node_block != 0 && node_block != INVALID_BLOCK) {
    ret = btree_read_node(sb, handle, node_block, &node);

    if (ret != 0)
      return ret;

    if (node->node->num_entries == 0) {
      if (node->node->is_leaf) {
        btree_free_node(node);
        return 0;
      }

      __u32 child_blk = bchildren(node, handle)[0];
      btree_free_node(node);
      node_block = child_blk;
      continue;
    }

    if (node->node->is_leaf)
      break;

    __u32 child_blk = bchildren(node, handle)[node->node->num_entries];

    // the max lives in the rightmost leaf. an internal entry is a separator copy of some leaf's first
    // entry, falling back to it would hand out an ino already in use.
    if (child_blk == 0 || child_blk == INVALID_BLOCK) {
      pr_err("inode tree: broken rightmost child at block %u\n", node_block);
      btree_free_node(node);
      return -EUCLEAN;
    }

    btree_free_node(node);
    node_block = child_blk;
  }

  if (!node || node->node->num_entries == 0) {
    if (node)
      btree_free_node(node);
    return 0;
  }

  struct disk_mapped_inode_t *inode = (struct disk_mapped_inode_t *)(bentries(node) + (node->node->num_entries - 1) * handle->entry_size);
  struct buffer_head         *bh    = node->bh;
  struct disk_mapped_inode   *max   = kmalloc(sizeof(struct disk_mapped_inode), GFP_NOFS);

  kfree(node);

  if (!max) {
    brelse(bh);
    return -ENOMEM;
  }

  max->inode = inode;
  max->bh    = bh;
  *out       = max;
  return 1;
}

// TODO: sync once and cache on sbi until unmount?
static void inode_sync_next(struct super_block *sb, btree_handle_t *inode_tree) {
  int                       ret;
  struct disk_mapped_inode *max_inode;
  struct bsfs_super_info   *sbi = bsfs_sb(sb);

  __u32 root_block = sbi->super->inode_root;

  if (root_block == 0 || !inode_tree) {
    sbi->next_inode_number = 1;
    return;
  }

  ret = inode_tree_find_max(sb, inode_tree, root_block, &max_inode);

  if (ret < 0) {
    // 0 makes get_next_inode_number report failure
    sbi->next_inode_number = 0;
    return;
  }

  if (ret == 1) {
    sbi->next_inode_number = max_inode->inode->ino + 1;
    brelse(max_inode->bh);
    kfree(max_inode);
  } else {
    sbi->next_inode_number = 1;
  }
}

__u32 get_next_inode_number(struct super_block *sb, btree_handle_t *inode_tree) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  inode_sync_next(sb, inode_tree);

  if (!sbi->next_inode_number)
    return 0;

  return sbi->next_inode_number++;
}

int insert_inode(struct super_block *sb, btree_handle_t *handle, const struct disk_mapped_inode_t *inode) {
  int ret;

  struct bsfs_super_info *sbi = bsfs_sb(sb);

  ret = btree_insert(sb, handle, (void *)inode);
  /* root moves on every cow insert */
  if (ret == 0 && handle->root_block != 0 && sbi->super->inode_root != handle->root_block) {
    sbi->super->inode_root = handle->root_block;
    mark_buffer_dirty(sbi->bh);
  }

  return ret;
}

// TODO: mark buffers dirty? locking? metadata?
void bsfs_truncate(struct inode *inode) {
  __u64              iblock;
  struct btree_node *node;

  struct super_block *sb         = inode->i_sb;
  struct bsfs_inode  *bsfs_inode = bsfs_i(inode);

  // if (new_size >= f->inode.file_size)
  //   return 0; // Nothing to truncate

  iblock = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

  block_truncate_page(inode->i_mapping, inode->i_size, bsfs_get_block);

  btree_handle_t *bspan_h = btree_create_bspan_handle();

  if (!bspan_h)
    return;

  // gives space back, may spend the whole reserve
  bspan_h->prio = BSFS_ALLOC_SHRINK;

  // after block_truncate_page, which goes through bsfs_get_block and takes this mutex
  mutex_lock(&bsfs_inode->truncate_mutex);

  bspan_h->root_block = bsfs_inode->btree_root;

  // shrink the span straddling the cut
  bspan_t span;
  int     found = bspan_search_le(sb, bspan_h, iblock, &span);

  if (found < 0)
    pr_warn("truncate: bspan lookup failed (%d), skipping head-span shrink\n", found);

  if (found == 1) {
    if (iblock < span.file_blk + span.length) {
      bspan_t tail     = span;
      __u32   keep_len = (iblock > span.file_blk) ? (iblock - span.file_blk) : 0;
      tail.file_blk    = iblock;
      tail.disk_blk    = span.disk_blk + keep_len;
      tail.length      = span.length - keep_len;

      // tree first, then defer the tail. the new root is only in the dirty inode, the on-disk
      // tree still reaches these blocks
      span.length = keep_len;
      if (keep_len > 0) {
        if (btree_update(sb, bspan_h, bspan_h->root_block, &span) == 1)
          bsfs_defer_free_bspan(sb, &tail);
        else
          pr_warn("truncate: failed to shrink span at file_blk %u, leaking %u blocks\n", span.file_blk, tail.length);
      } else {
        if (btree_remove(sb, bspan_h, span.file_blk))
          bsfs_defer_free_bspan(sb, &tail);
        else
          pr_warn("truncate: failed to remove span at file_blk %u, leaking %u blocks\n", span.file_blk, tail.length);
      }
    }
  }

  // remove every span at or past iblock. keys[] is on the stack, so a fragmented file takes several
  // passes. loop until a pass finds nothing above the cut.
  __u32  keys[64];
  size_t kcount;

  // 12 byte bspans give the widest fan-out, 2t = 256. if the stack fills anyway the sweep just
  // runs another pass.
  __u32 stack[256];
  int   top;

  for (;;) {
    size_t removed = 0;

    kcount = 0;
    top    = 0;

    if (bspan_h->root_block)
      stack[top++] = bspan_h->root_block;

    while (top > 0 && kcount < ARRAY_SIZE(keys)) {
      int   ret;
      __u32 nb = stack[--top];

      ret = btree_read_node(sb, bspan_h, nb, &node);

      if (ret != 0)
        continue;

      if (!node->node->is_leaf) {
        for (__u32 i = 0; i <= node->node->num_entries; i++) {
          if (top < (int)ARRAY_SIZE(stack))
            stack[top++] = bchildren(node, bspan_h)[i];
          else
            pr_warn("truncate: DFS stack full, sweeping again\n");
        }
      }

      // internal entries are separator copies, would queue keys twice
      if (node->node->is_leaf) {
        for (__u32 i = 0; i < node->node->num_entries && kcount < ARRAY_SIZE(keys); i++) {
          __u32 ekey = bspan_h->get_key(bentries(node) + i * bspan_h->entry_size);

          if (ekey >= iblock)
            keys[kcount++] = ekey;
        }
      }

      btree_free_node(node);
    }

    if (kcount == 0)
      break;

    for (size_t i = 0; i < kcount; i++) {
      bspan_t *found;
      bspan_t  bs;

      // found points into the node buffer, copy before freeing it
      if (btree_search(sb, bspan_h, bspan_h->root_block, keys[i], (void **)&found, &node) == 1) {
        bs = *found;
        btree_free_node(node);
        // unlink from the tree first, then defer the blocks
        if (btree_remove(sb, bspan_h, keys[i])) {
          bsfs_defer_free_bspan(sb, &bs);
          removed++;
        } else {
          pr_warn("truncate: failed to remove span at file_blk %u, leaking %u blocks\n", bs.file_blk, bs.length);
        }
      }
    }

    // otherwise we'd sweep forever
    if (removed == 0) {
      pr_warn("truncate: %zu spans past the cut could not be removed\n", kcount);
      break;
    }
  }

  // removals may have moved or emptied the root
  if (bsfs_inode->btree_root != bspan_h->root_block) {
    bsfs_inode->btree_root = bspan_h->root_block;
    mark_inode_dirty(inode);
  }

  mutex_unlock(&bsfs_inode->truncate_mutex);
  kfree(bspan_h);
}

struct disk_mapped_inode *bsfs_read_disk_inode(struct super_block *sb, ino_t ino) {
  int ret;

  struct bsfs_super_info     *sbi = sb->s_fs_info;
  struct btree_node          *node;
  struct disk_mapped_inode_t *inode;

  btree_handle_t *inode_h = btree_create_inode_handle();

  if (!inode_h)
    return NULL;

  inode_h->root_block = sbi->super->inode_root;

  ret = btree_search(sb, inode_h, inode_h->root_block, ino, (void **)&inode, &node);
  kfree(inode_h);

  if (ret != 1)
    return NULL;

  // keep the bh, caller brelses it
  struct buffer_head *bh = node->bh;
  kfree(node);

  struct disk_mapped_inode *out = kmalloc(sizeof(struct disk_mapped_inode), GFP_NOFS);

  if (!out) {
    brelse(bh);
    return NULL;
  }

  out->inode = inode;
  out->bh    = bh;

  return out;
}

/*
 * goes through btree_update(), never the pointer from bsfs_read_disk_inode(). that points into a
 * node buffer and the next cow mutation releases that block to somebody else.
 */
static int bsfs_update_inode(struct inode *inode) {
  struct super_block        *sb  = inode->i_sb;
  struct bsfs_super_info    *sbi = bsfs_sb(sb);
  struct disk_mapped_inode  *current_inode;
  struct disk_mapped_inode_t entry;
  btree_handle_t            *inode_h;
  int                        ret = 0;

  inode_h = btree_create_inode_handle();

  if (!inode_h)
    return -ENOMEM;

  // outranks growth since it unblocks deferred frees, but not the shrink paths
  inode_h->prio = BSFS_ALLOC_WRITEBACK;

  mutex_lock(&sbi->inode_tree_lock);

  inode_h->root_block = sbi->super->inode_root;

  // copy, fields the vfs doesn't own (block count, padding) must survive
  current_inode = bsfs_read_disk_inode(sb, inode->i_ino);

  if (!current_inode) {
    ret = -EIO;
    goto out;
  }

  entry = *current_inode->inode;
  brelse(current_inode->bh);
  kfree(current_inode);

  entry.ino        = inode->i_ino;
  entry.mode       = inode->i_mode;
  entry.uid        = fs_high2lowuid(i_uid_read(inode));
  entry.gid        = fs_high2lowgid(i_gid_read(inode));
  entry.nlinks     = inode->i_nlink;
  entry.size       = inode->i_size;
  entry.mtime      = inode_get_mtime_sec(inode);
  entry.atime      = inode_get_atime_sec(inode);
  entry.ctime      = inode_get_ctime_sec(inode);
  entry.btree_root = bsfs_i(inode)->btree_root;

  // TODO: special files
  // if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
  //   raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);

  if (btree_update(sb, inode_h, inode_h->root_block, &entry) != 1) {
    ret = -EIO;
    goto out;
  }

  // btree_update() publishes a new root every call
  if (inode_h->root_block != 0 && sbi->super->inode_root != inode_h->root_block) {
    sbi->super->inode_root = inode_h->root_block;
    mark_buffer_dirty(sbi->bh);
  }

out:
  mutex_unlock(&sbi->inode_tree_lock);
  kfree(inode_h);
  return ret;
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
    printk(KERN_DEBUG "BSFS: Invalid file type 0%04o for inode %llu.\n", inode->i_mode, inode->i_ino);
    make_bad_inode(inode);
  }
}

static struct inode *__set_up_inode_new(struct inode *inode) {
  struct disk_mapped_inode *disk_inode;
  struct bsfs_super_info   *sbi = bsfs_sb(inode->i_sb);

  // disk_inode->inode points into a node buffer, hold the lock while reading it
  mutex_lock(&sbi->inode_tree_lock);

  disk_inode = bsfs_read_disk_inode(inode->i_sb, inode->i_ino);

  struct bsfs_inode *bsfs_inode = bsfs_i(inode);

  if (!disk_inode) {
    mutex_unlock(&sbi->inode_tree_lock);
    iget_failed(inode);
    return ERR_PTR(-EIO);
  }

  if (disk_inode->inode->nlinks == 0) {
    printk("BSFS: deleted inode referenced: %llu\n", inode->i_ino);
    mutex_unlock(&sbi->inode_tree_lock);
    brelse(disk_inode->bh);
    kfree(disk_inode);
    iget_failed(inode);
    return ERR_PTR(-ESTALE);
  }

  inode->i_mode = disk_inode->inode->mode;
  i_uid_write(inode, disk_inode->inode->uid);
  i_gid_write(inode, disk_inode->inode->gid);
  set_nlink(inode, disk_inode->inode->nlinks);

  i_size_write(inode, disk_inode->inode->size);

  inode_set_mtime(inode, disk_inode->inode->mtime, 0);
  inode_set_atime(inode, disk_inode->inode->atime, 0);
  inode_set_ctime(inode, disk_inode->inode->ctime, 0);

  // TODO: maintain i_blocks
  inode->i_blocks = 0;

  bsfs_inode->btree_root = disk_inode->inode->btree_root;

  // TODO: special file inodes
  // bsfs_set_inode(inode, old_decode_dev(rdev));

  bsfs_set_inode(inode, 0);

  mutex_unlock(&sbi->inode_tree_lock);
  brelse(disk_inode->bh);
  kfree(disk_inode);

  unlock_new_inode(inode);
  return inode;
}

struct inode *bsfs_get_vfs_inode(struct super_block *sb, unsigned long ino) {
  struct inode *inode;

  inode = iget_locked(sb, ino);

  if (!inode)
    return ERR_PTR(-ENOMEM);

  if (!(inode_state_read(inode) & I_NEW))
    return inode;

  return __set_up_inode_new(inode);
}

struct inode *bsfs_alloc_inode(struct super_block *sb) {
  struct bsfs_inode *bsfs_inode;

  // init_once() only runs per slab object, not per reuse
  bsfs_inode = alloc_inode_sb(sb, bsfs_inode_cache, GFP_KERNEL);

  if (!bsfs_inode)
    return NULL;

  // recycled slab, clear the snapshot-pending sentinel
  bsfs_inode->i_deleted_parent_ino = 0;

  return &bsfs_inode->vfs_inode;
}

int bsfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
  struct bsfs_super_info *sbi = bsfs_sb(inode->i_sb);
  int                     ret;

  ret = bsfs_update_inode(inode);

  if (ret != 0) {
    // root is memory-only, so the commit must not recycle what it replaced
    mutex_lock(&sbi->pending_free_lock);
    // a hard error beats a stored ENOSPC
    if (!sbi->writeback_error || (sbi->writeback_error == -ENOSPC && ret != -ENOSPC))
      sbi->writeback_error = ret;
    mutex_unlock(&sbi->pending_free_lock);

    return ret;
  }

  // no single node buffer to sync after cow, only the root has to be durable. nodes go out via
  // bsfs_fsync()'s sync_blockdev() and normal bdev writeback.
  if (wbc->sync_mode == WB_SYNC_ALL && buffer_dirty(sbi->bh))
    sync_dirty_buffer(sbi->bh);

  return ret;
}

struct inode *bsfs_new_inode(const struct inode *dir, umode_t mode) {
  struct super_block     *sb  = dir->i_sb;
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  struct inode              *inode;
  struct bsfs_inode         *bsfs_inode;
  struct disk_mapped_inode_t disk_inode = {0};
  btree_handle_t            *inode_h;
  int                        err;

  inode = new_inode(sb);
  if (!inode)
    return ERR_PTR(-ENOMEM);

  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
  simple_inode_init_ts(inode);
  inode->i_blocks = 0;

  inode_h = btree_create_inode_handle();

  if (!inode_h) {
    err = -ENOMEM;
    goto err_iput;
  }

  // ino pick and insert under one lock, or two creators race to the same ino
  mutex_lock(&sbi->inode_tree_lock);

  inode_h->root_block = sbi->super->inode_root;

  inode->i_ino = get_next_inode_number(sb, inode_h);
  if (!inode->i_ino) {
    mutex_unlock(&sbi->inode_tree_lock);
    err = -ENOSPC;
    goto err_free_handle;
  }

  disk_inode.ino = inode->i_ino;

  if (insert_inode(sb, inode_h, &disk_inode) != 0) {
    mutex_unlock(&sbi->inode_tree_lock);
    err = -ENOSPC;
    goto err_free_handle;
  }

  mutex_unlock(&sbi->inode_tree_lock);
  kfree(inode_h);

  bsfs_inode             = bsfs_i(inode);
  bsfs_inode->btree_root = 0;

  insert_inode_hash(inode);
  mark_inode_dirty(inode);

  return inode;

err_free_handle:
  kfree(inode_h);
err_iput:
  iput(inode);
  return ERR_PTR(err);
}

void bsfs_free_cache_inode(struct inode *inode) {
  kmem_cache_free(bsfs_inode_cache, bsfs_i(inode));
}

void bsfs_evict_inode(struct inode *inode) {
  bool want_delete = !inode->i_nlink && !is_bad_inode(inode);
  bool snapshot    = false;
  // btree_root is a dentry tree for dirs, a span tree for files. read before clear_inode()
  bool is_dir      = S_ISDIR(inode->i_mode);

  struct super_block     *sb  = inode->i_sb;
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  struct bsfs_inode      *bi  = bsfs_i(inode);

  truncate_inode_pages_final(&inode->i_data);

  if (want_delete) {
    // no truncate like other fs, the recovery snapshot needs the data on disk.
    // a stashed parent ino means we got here via unlink/rmdir/rename, so it's recoverable
    if (bi->i_deleted_parent_ino) {
      btree_handle_t *rec_tree = btree_create_rec_entry_handle();

      if (rec_tree) {
        rec_entry_t rec = {0};

        // bsfs_rec_tree_add assigns recovery_id
        rec.deleted_at          = ktime_get_real_seconds();
        rec.original_parent_ino = bi->i_deleted_parent_ino;

        strscpy(rec.original_name, bi->i_deleted_name, sizeof(rec.original_name));

        rec.inode_number    = inode->i_ino;
        rec.file_type       = S_ISDIR(inode->i_mode) ? BSFS_REC_TYPE_DIR : BSFS_REC_TYPE_FILE;
        rec.uid             = fs_high2lowuid(i_uid_read(inode));
        rec.gid             = fs_high2lowgid(i_gid_read(inode));
        rec.permissions     = inode->i_mode;
        rec.file_size       = inode->i_size;
        rec.block_count     = 0; // not maintained on disk, reclaim walks btree_root
        rec.btree_root      = bi->btree_root;
        rec.retention_until = rec.deleted_at + DEFAULT_RECOVERY_TTL;

        rec_tree->root_block = sbi->super->recovery_root;

        mutex_lock(&sbi->rec_lock);

        // persists recovery_root itself
        if (bsfs_rec_tree_add(sb, rec_tree, &rec))
          pr_warn("failed to record recovery snapshot for inode %llu\n", inode->i_ino);
        else
          snapshot = true; // snapshot owns the blocks now

        mutex_unlock(&sbi->rec_lock);

        kfree(rec_tree);
      } else {
        pr_warn("no memory for recovery snapshot of inode %llu\n", inode->i_ino);
      }
    }
  }

  clear_inode(inode);

  if (want_delete) {
    btree_handle_t *inode_h;

    // no snapshot, so the tree and every block it maps are unreachable. free them or they leak
    // until the next mkfs
    if (!snapshot && bi->btree_root) {
      // a dentry tree walked as spans would free garbage
      btree_handle_t *tree_h = is_dir ? btree_create_dir_entry_handle() : btree_create_bspan_handle();

      if (tree_h) {
        tree_h->prio       = BSFS_ALLOC_SHRINK;
        tree_h->root_block = bi->btree_root;

        if (btree_deallocate_all(sb, tree_h, tree_h->root_block, NULL) != 0)
          pr_warn("failed to reclaim the tree of inode %llu, leaking its blocks\n", inode->i_ino);

        kfree(tree_h);
      } else {
        pr_warn("no memory to reclaim the tree of inode %llu\n", inode->i_ino);
      }

      bi->btree_root = 0;
    }

    inode_h = btree_create_inode_handle();

    if (inode_h) {
      mutex_lock(&sbi->inode_tree_lock);

      inode_h->prio       = BSFS_ALLOC_SHRINK;
      inode_h->root_block = sbi->super->inode_root;
      btree_remove(sb, inode_h, inode->i_ino);
      if (sbi->super->inode_root != inode_h->root_block) {
        sbi->super->inode_root = inode_h->root_block;
        mark_buffer_dirty(sbi->bh);
      }

      mutex_unlock(&sbi->inode_tree_lock);
      kfree(inode_h);
    } else {
      pr_warn("no memory to remove inode %llu from inode tree\n", inode->i_ino);
    }
  }

}

static void init_once(void *foo) {
  struct bsfs_inode *ei = foo;

  mutex_init(&ei->truncate_mutex);
  inode_init_once(&ei->vfs_inode);
}

int __init init_inodecache(void) {
  bsfs_inode_cache = kmem_cache_create("bsfs_inode_cache", sizeof(struct bsfs_inode), 0, (SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT), init_once);

  if (bsfs_inode_cache == NULL)
    return -ENOMEM;

  return 0;
}

void destroy_inodecache(void) {
  /* flush rcu-delayed inode frees first */
  rcu_barrier();
  kmem_cache_destroy(bsfs_inode_cache);
}

int bsfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh, int create) {
  int found = 0;
  int ret   = 0;

  struct super_block *sb = inode->i_sb;

  struct bsfs_inode *bsfs_inode = bsfs_i(inode);

  btree_handle_t *bspan_h;
  bspan_t         span;

  bspan_h = btree_create_bspan_handle();

  if (!bspan_h)
    return -ENOMEM;

  mutex_lock(&bsfs_inode->truncate_mutex);

  bspan_h->root_block = bsfs_inode->btree_root;

  found = bspan_search_le(sb, bspan_h, iblock, &span);

  // the block may already be mapped, allocating would shadow it
  if (found < 0) {
    ret = found;
    goto out;
  }

  if (found == 1 && iblock >= span.file_blk && iblock < span.file_blk + span.length) {
    map_bh(bh, sb, span.disk_blk + (iblock - span.file_blk));
    goto out;
  }

  if (!create)
    goto out;

  __u32 phys_block = allocate_single_block(sb);

  if (phys_block == INVALID_BLOCK) {
    ret = -ENOSPC;
    goto out;
  }

  // likely a btree node a moment ago, its bh may still be dirty in the bdev mapping. file data goes
  // through the inode mapping, so without this both writebacks hit the same block and the last wins.
  clean_bdev_aliases(sb->s_bdev, phys_block, 1);

  // extend the previous span if contiguous. length is __u16, a wrap to 0 would stop matching and
  // insert a second span at the same file_blk
  if (found == 1 && span.length < BSFS_MAX_SPAN_BLOCKS && iblock == span.file_blk + span.length &&
      span.disk_blk + span.length == phys_block) {
    span.length++;
    if (btree_update(sb, bspan_h, bspan_h->root_block, &span) != 1) {
      deallocate_single_block(sb, phys_block);
      ret = -EIO;
      goto out;
    }
  } else {
    bspan_t new_span = {
        .file_blk = iblock,
        .disk_blk = phys_block,
        .length   = 1,
    };

    if (btree_insert(sb, bspan_h, &new_span) != 0) {
      deallocate_single_block(sb, phys_block);
      ret = -ENOSPC;
      goto out;
    }
  }

  if (bsfs_inode->btree_root != bspan_h->root_block) {
    bsfs_inode->btree_root = bspan_h->root_block;
    mark_inode_dirty(inode);
  }

  map_bh(bh, sb, phys_block);
  set_buffer_new(bh);

out:
  mutex_unlock(&bsfs_inode->truncate_mutex);
  kfree(bspan_h);
  return ret;
}

static int bsfs_read_folio(struct file *file, struct folio *folio) {
  return block_read_full_folio(folio, bsfs_get_block);
}

static void bsfs_write_failed(struct address_space *mapping, loff_t to) {
  struct inode *inode = mapping->host;

  if (to > inode->i_size) {
    truncate_pagecache(inode, inode->i_size);
    bsfs_truncate(inode);
  }
}

/*
 * allocates here, not at writeback. delalloc via buffer_delay doesn't work with mpage_writepages(),
 * it ignores BH_Delay and wrote data to block 0. real delalloc needs our own ->writepages (mpage_da_*).
 */
static int bsfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop,
                            void **fsdata) {
  int ret;

  // cheap, keeps the common case away from the retry
  if (bsfs_should_reclaim(mapping->host->i_sb))
    (void)bsfs_reclaim_space(mapping->host->i_sb);

  ret = block_write_begin(mapping, pos, len, foliop, bsfs_get_block);

  // ENOSPC may just be the deferred list. block_write_begin() dropped the folio and we hold no
  // tree lock, so run the barrier and retry.
  if (ret == -ENOSPC && bsfs_reclaim_space(mapping->host->i_sb) == 0)
    ret = block_write_begin(mapping, pos, len, foliop, bsfs_get_block);

  if (unlikely(ret))
    bsfs_write_failed(mapping, pos + len);

  return ret;
}

static int bsfs_writepages(struct address_space *mapping, struct writeback_control *wbc) {
  struct inode *inode = mapping->host;
  struct folio *folio = NULL;
  int           error = 0;

  while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
    loff_t i_size = i_size_read(inode);

    // past eof, racing truncate
    if (folio_pos(folio) >= i_size) {
      folio_unlock(folio);
      error = 0;
      continue;
    }

    // re-zero the tail past eof on every writeback, the folio may be mmapped
    if (folio_pos(folio) + folio_size(folio) > i_size)
      folio_zero_segment(folio, offset_in_folio(folio, i_size), folio_size(folio));

    error = __block_write_full_folio(inode, folio, bsfs_get_block, wbc);
  }

  return error;
}

static sector_t bsfs_bmap(struct address_space *mapping, sector_t block) {
  return generic_block_bmap(mapping, block, bsfs_get_block);
}

const struct address_space_operations bsfs_aops = {
    .write_begin      = bsfs_write_begin,
    .write_end        = generic_write_end,
    .writepages       = bsfs_writepages,
    .read_folio       = bsfs_read_folio,
    .dirty_folio      = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
    .migrate_folio    = buffer_migrate_folio,
    .bmap             = bsfs_bmap,
};
