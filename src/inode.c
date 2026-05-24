#include "../include/inode.h"
#include "../bsfs.h"
#include "../include/btree.h"
#include "../include/dir.h"
#include "../include/file.h"
#include <linux/writeback.h>

static __u32 s_next_inode_number = 1;
// static __u32 s_cached_inode_root = INVALID_BLOCK;

static struct kmem_cache *bsfs_inode_cache;

// TODO: is this working?
static struct disk_mapped_inode *inode_tree_find_max(struct super_block *sb, btree_handle_t *handle, __u32 node_block) {
  struct btree_node *node = NULL;

  if (!handle || node_block == 0 || node_block == INVALID_BLOCK)
    return NULL;

  while (node_block != 0 && node_block != INVALID_BLOCK) {
    if (btree_read_node(sb, node_block, &node) != 0 || !node)
      return NULL;

    if (node->node->num_entries == 0) {
      if (node->node->is_leaf) {
        btree_free_node(node);
        return NULL;
      }

      __u32 child_blk = bchildren(node, handle)[0];
      btree_free_node(node);
      node_block = child_blk;
      continue;
    }

    if (node->node->is_leaf)
      break;

    __u32 child_idx = node->node->num_entries;
    if (child_idx >= 2 * handle->min_degree)
      child_idx = node->node->num_entries - 1;

    __u32 child_blk = bchildren(node, handle)[child_idx];
    if (child_blk == 0 || child_blk == INVALID_BLOCK)
      break;

    btree_free_node(node);
    node_block = child_blk;
  }

  if (!node || node->node->num_entries == 0) {
    if (node)
      btree_free_node(node);
    return NULL;
  }

  struct disk_mapped_inode_t *inode =
      (struct disk_mapped_inode_t *)(bentries(node) + (node->node->num_entries - 1) * handle->entry_size);
  struct buffer_head *bh = node->bh;
  struct disk_mapped_inode *out = kmalloc(sizeof(struct disk_mapped_inode), GFP_NOFS);

  kfree(node);

  if (!out) {
    brelse(bh);
    return NULL;
  }

  out->inode = inode;
  out->bh    = bh;
  return out;
}

static void inode_sync_next(struct super_block *sb, btree_handle_t *inode_tree) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);

  __u32 root_block = inode_tree ? inode_tree->root_block : 0;

  root_block = sbi->super->inode_root;

  if (root_block == 0 || !inode_tree) {
    s_next_inode_number = 1;
    return;
  }

  struct disk_mapped_inode *max_inode = inode_tree_find_max(sb, inode_tree, root_block);
  if (max_inode) {
    s_next_inode_number = max_inode->inode->ino + 1;
    brelse(max_inode->bh);
    kfree(max_inode);
  } else {
    s_next_inode_number = 1;
  }
  s_cached_inode_root = root_block;
}

__u32 get_next_inode_number(struct super_block *sb, btree_handle_t *inode_tree) {
  inode_sync_next(disk, sb, inode_tree);

  if (s_next_inode_number == 0)
    s_next_inode_number = 1;

  return s_next_inode_number++;
}

int insert_inode(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, const bsfs_inode *inode) {
  if (!handle || !inode)
    return -1;

  bsfs_inode copy;
  memcpy(&copy, inode, sizeof(bsfs_inode));
  int rc = btree_insert(disk, sb, handle, &copy);
  /* Persist inode root whenever it changes (e.g., after a split). */
  if (rc == 0 && handle->root_block != 0 && sb->inode_root != handle->root_block) {
    sb->inode_root = handle->root_block;
    (void)write_superblock(disk, sb);
  }
  if (rc == 0) {
    if (inode->inode_number >= s_next_inode_number)
      s_next_inode_number = inode->inode_number + 1;
    s_cached_inode_root = handle->root_block;
  }
  return rc;
}

int update_inode(FILE *disk, btree_handle_t *handle, const bsfs_inode *inode) {
  if (!handle || !inode)
    return -1;
  int ok = btree_update(disk, handle, handle->root_block, (void *)inode);
  return ok;
}

int remove_inode(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, uint32_t inode_number) {
  if (!handle || handle->type != BTREE_TYPE_INODE)
    return -1;
  int removed = btree_remove(disk, sb, handle, inode_number);
  if (!removed && handle) {
    bsfs_inode tmp;
    int        present = btree_search(disk, handle, handle->root_block, inode_number, &tmp);
    if (present) {
      if (btree_remove_entry(disk, sb, handle, &tmp)) {
        removed = 1;
      }
    }
    if (!removed) {
      fprintf(stderr, "remove_inode: failed to purge ino=%u (root=%u sb_root=%u present=%d)\n", inode_number, handle->root_block,
              sb ? sb->inode_root : 0, present);
    }
  }
  if (removed && sb->inode_root != handle->root_block) {
    sb->inode_root = handle->root_block;
    (void)write_superblock(disk, sb);
  }
  if (removed)
    inode_sync_next(disk, sb, handle);
  return removed;
}

int inode_exists(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number) {
  bsfs_inode temp;
  return btree_search(disk, inode_tree, inode_tree->root_block, inode_number, &temp);
}

// int inode_tree_init(FILE *disk, bsfs_super_block *sb, btree_handle_t *out) {
//   if (!disk || !sb || !out)
//     return 0;
//
//   btree_handle_t *tmp = btree_create_inode_handle();
//   if (!tmp)
//     return 0;
//
//   if (sb->inode_root == 0) {
//     // Create empty root leaf node on disk
//     btree_node_t node;
//     node.is_leaf     = 1;
//     node.num_entries = 0;
//     node.entries     = calloc((size_t)(2 * tmp->min_degree - 1), (size_t)tmp->entry_size);
//     node.children    = calloc((size_t)(2 * tmp->min_degree), sizeof(uint32_t));
//     if (!node.entries || !node.children) {
//       free(node.entries);
//       free(node.children);
//       free(tmp);
//       return 0;
//     }
//
//     uint32_t root_block = allocate_single_block(disk, sb);
//     if (root_block == INVALID_BLOCK || root_block == 0) {
//       free(node.entries);
//       free(node.children);
//       free(tmp);
//       return 0;
//     }
//
//     tmp->root_block = root_block;
//     if (btree_write_node(disk, tmp, root_block, &node) != 0) {
//       free(node.entries);
//       free(node.children);
//       free(tmp);
//       return 0;
//     }
//     free(node.entries);
//     free(node.children);
//
//     sb->inode_root = root_block;
//     if (write_superblock(disk, sb) != 0) {
//       free(tmp);
//       return 0;
//     }
//   } else {
//     tmp->root_block = sb->inode_root;
//   }
//
//   *out = *tmp;
//   free(tmp);
//   return 1;
// }

static inline int bsfs_buffer_uptodate(struct buffer_head *bh) {
  /*
   * If the buffer has the write error flag, we have failed
   * to write out data in the block.  In this  case, we don't
   * have to read the block because we may read the old data
   * successfully.
   */
  if (buffer_write_io_error(bh))
    set_buffer_uptodate(bh);
  return buffer_uptodate(bh);
}

static inline void __bsfs_read_bh(struct buffer_head *bh, blk_opf_t op_flags, bh_end_io_t *end_io) {

  bh->b_end_io = end_io ? end_io : end_buffer_read_sync;
  get_bh(bh);
  submit_bh(REQ_OP_READ | op_flags, bh);
}

void bsfs_read_bh_nowait(struct buffer_head *bh, blk_opf_t op_flags, bh_end_io_t *end_io) {
  BUG_ON(!buffer_locked(bh));

  if (bsfs_buffer_uptodate(bh)) {
    unlock_buffer(bh);
    return;
  }

  __bsfs_read_bh(bh, op_flags, end_io);
}

int bsfs_read_bh(struct buffer_head *bh, blk_opf_t op_flags, bh_end_io_t *end_io) {
  BUG_ON(!buffer_locked(bh));

  if (bsfs_buffer_uptodate(bh)) {
    unlock_buffer(bh);
    return 0;
  }

  __bsfs_read_bh(bh, op_flags, end_io);

  wait_on_buffer(bh);

  if (buffer_uptodate(bh))
    return 0;

  return -EIO;
}

int bsfs_read_bh_lock(struct buffer_head *bh, blk_opf_t op_flags, bool wait) {
  lock_buffer(bh);
  if (!wait) {
    bsfs_read_bh_nowait(bh, op_flags, NULL);
    return 0;
  }
  return bsfs_read_bh(bh, op_flags, NULL);
}

int bsfs_block_write_begin(struct folio *folio, loff_t pos, unsigned len, get_block_t *get_block) {
  unsigned int        from  = offset_in_folio(folio, pos);
  unsigned            to    = from + len;
  struct inode       *inode = folio->mapping->host;
  unsigned            block_start, block_end;
  sector_t            block;
  int                 err       = 0;
  unsigned            blocksize = inode->i_sb->s_blocksize;
  unsigned            bbits;
  struct buffer_head *bh, *head, *wait[2];
  int                 nr_wait = 0;
  int                 i;

  BUG_ON(!folio_test_locked(folio));
  BUG_ON(to > folio_size(folio));
  BUG_ON(from > to);

  head = folio_buffers(folio);
  if (!head)
    head = create_empty_buffers(folio, blocksize, 0);
  bbits = ilog2(blocksize);
  block = (sector_t)folio->index << (PAGE_SHIFT - bbits);

  for (bh = head, block_start = 0; bh != head || !block_start; block++, block_start = block_end, bh = bh->b_this_page) {
    block_end = block_start + blocksize;
    if (block_end <= from || block_start >= to) {
      if (folio_test_uptodate(folio)) {
        set_buffer_uptodate(bh);
      }
      continue;
    }
    if (WARN_ON_ONCE(buffer_new(bh)))
      clear_buffer_new(bh);
    if (!buffer_mapped(bh)) {
      WARN_ON(bh->b_size != blocksize);

      err = get_block(inode, block, bh, 1);

      if (err)
        break;

      if (buffer_new(bh)) {
        if (folio_test_uptodate(folio)) {
          /*
           * Unlike __block_write_begin() we leave
           * dirtying of new uptodate buffers to
           * ->write_end() time or
           * folio_zero_new_buffers().
           */
          set_buffer_uptodate(bh);
          continue;
        }
        if (block_end > to || block_start < from)
          folio_zero_segments(folio, to, block_end, block_start, from);
        continue;
      }
    }
    if (folio_test_uptodate(folio)) {
      set_buffer_uptodate(bh);
      continue;
    }
    if (!buffer_uptodate(bh) && !buffer_delay(bh) && !buffer_unwritten(bh) && (block_start < from || block_end > to)) {
      bsfs_read_bh_lock(bh, 0, false);
      wait[nr_wait++] = bh;
    }
  }
  /*
   * If we issued read requests, let them complete.
   */
  for (i = 0; i < nr_wait; i++) {
    wait_on_buffer(wait[i]);
    if (!buffer_uptodate(wait[i]))
      err = -EIO;
  }

  if (unlikely(err)) {
    folio_zero_new_buffers(folio, from, to);
  }

  return err;
}

// TODO: mark buffers dirty?
// TODO: locking?
// TODO: update metadata?
void bsfs_truncate(struct inode *inode) {
  __u64              iblock;
  struct btree_node *node;

  struct super_block *sb         = inode->i_sb;
  struct bsfs_inode  *bsfs_inode = bsfs_i(inode);

  // if (new_size >= f->inode.file_size)
  //   return 0; // Nothing to truncate

  // starting block to free during truncate
  iblock = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;

  block_truncate_page(inode->i_mapping, inode->i_size, bsfs_get_block);

  btree_handle_t *bspan_h = btree_create_bspan_handle();
  bspan_h->root_block     = bsfs_inode->btree_root;

  // 1) Shrink overlapping head span if needed
  bspan_t span;
  if (bspan_search_le(sb, bspan_h, iblock, &span)) {
    if (iblock < span.file_blk + span.length) {
      // free tail portion beyond new_size
      bspan_t tail     = span;
      __u32   keep_len = (iblock > span.file_blk) ? (iblock - span.file_blk) : 0;
      tail.file_blk    = iblock;
      tail.disk_blk    = span.disk_blk + keep_len;
      tail.length      = span.length - keep_len;

      if (tail.length > 0)
        deallocate_bspan(sb, &tail);

      // shrink original span to keep portion
      span.length = keep_len;
      if (keep_len > 0) {
        btree_update(sb, bspan_h, bspan_h->root_block, &span);
      } else {
        // remove empty span entry
        btree_remove(sb, bspan_h, span.file_blk);
      }
    }
  }

  // 2) Collect and remove all spans starting at or after iblock
  // Traverse tree and collect keys >= threshold
  typedef struct {
    __u32 k;
  } krec;

  krec   keys[1024];
  size_t kcount = 0;
  // simple DFS stack of node blocks
  __u32 stack[128];
  int   top = 0;

  if (bspan_h->root_block)
    stack[top++] = bspan_h->root_block;

  while (top > 0) {
    int   ret;
    __u32 nb = stack[--top];

    ret = btree_read_node(sb, nb, &node);

    if (ret != 0)
      continue;

    // push children first for post-order like
    if (!node->node->is_leaf) {
      for (__u32 i = 0; i <= node->node->num_entries; i++) {
        if (top < (int)(sizeof(stack) / sizeof(stack[0])))
          stack[top++] = bchildren(node, bspan_h)[i];
      }
    }

    for (__u32 i = 0; i < node->node->num_entries; i++) {
      __u32 ekey = bspan_h->get_key(bentries(node) + i * bspan_h->entry_size);
      if (ekey >= iblock) {
        if (kcount < sizeof(keys) / sizeof(keys[0])) {
          keys[kcount++].k = ekey;
        }
      }
    }

    btree_free_node(node);
  }

  // Remove collected keys
  for (size_t i = 0; i < kcount; i++) {
    bspan_t bs;
    if (btree_search(sb, bspan_h, bspan_h->root_block, keys[i].k, (void **)&bs, &node)) {
      btree_free_node(node);
      deallocate_bspan(sb, &bs);
      btree_remove(sb, bspan_h, keys[i].k);
    }
  }
}

// old read_inode() on btree.c
struct disk_mapped_inode *__read_disk_inode(struct super_block *sb, ino_t ino) {
  int ret;

  struct bsfs_super_info     *sbi = sb->s_fs_info;
  struct btree_node          *node;
  struct disk_mapped_inode_t *inode;

  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block     = sbi->super->inode_root;

  ret = btree_search(sb, inode_h, inode_h->root_block, ino, (void **)&inode, &node);

  if (!ret)
    return NULL;

  // TODO: applicable?
  // if (!ino || ino > sbi->s_ninodes) {
  //   printk("Bad inode number on dev %s: %ld is out of range\n", sb->s_id, (long)ino);
  //   return NULL;
  // }

  struct disk_mapped_inode *out = kmalloc(sizeof(struct disk_mapped_inode), GFP_NOFS);

  // free btree_node without releasing buffer
  // buffer will be brelsed later
  struct buffer_head *bh = node->bh;
  kfree(node);

  out->inode = inode;
  out->bh    = bh;

  return out;
}

struct disk_mapped_inode *bsfs_update_inode(struct inode *inode) {
  struct disk_mapped_inode *disk_inode;

  disk_inode = __read_disk_inode(inode->i_sb, inode->i_ino);

  if (!disk_inode)
    return NULL;

  disk_inode->inode->mode   = inode->i_mode;
  disk_inode->inode->uid    = fs_high2lowuid(i_uid_read(inode));
  disk_inode->inode->gid    = fs_high2lowgid(i_gid_read(inode));
  disk_inode->inode->nlinks = inode->i_nlink;
  disk_inode->inode->size   = inode->i_size;
  disk_inode->inode->mtime  = inode_get_mtime_sec(inode);
  disk_inode->inode->atime  = inode_get_atime_sec(inode);
  disk_inode->inode->ctime  = inode_get_ctime_sec(inode);

  // TODO: make it work with special files
  // if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
  //   raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);

  mark_buffer_dirty(disk_inode->bh);
  return disk_inode;
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
  struct disk_mapped_inode *disk_inode;

  disk_inode = __read_disk_inode(inode->i_sb, inode->i_ino);

  struct bsfs_inode *bsfs_inode = bsfs_i(inode);

  if (!disk_inode) {
    iget_failed(inode);
    return ERR_PTR(-EIO);
  }

  if (disk_inode->inode->nlinks == 0) {
    printk("BSFS: deleted inode referenced: %lu\n", inode->i_ino);
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

  // TODO: whats this?
  inode->i_blocks = 0;

  bsfs_inode->btree_root = disk_inode->inode->btree_root;

  // TODO: support special file inodes
  // bsfs_set_inode(inode, old_decode_dev(rdev));

  bsfs_set_inode(inode, 0);

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

  if (!(inode->i_state & I_NEW))
    return inode;

  return __set_up_inode_new(inode);
}

static struct inode *bsfs_alloc_inode(struct super_block *sb) {
  struct bsfs_inode *bsfs_inode;

  bsfs_inode = alloc_inode_sb(sb, bsfs_inode_cache, GFP_KERNEL);

  if (!bsfs_inode)
    return NULL;

  return &bsfs_inode->vfs_inode;
}

int bsfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
  int ret = 0;

  struct disk_mapped_inode *disk_inode;
  struct buffer_head       *bh;

  disk_inode = bsfs_update_inode(inode);

  if (!disk_inode) {
    ret = -EIO;
    goto out;
  }

  bh = disk_inode->bh;

  if (wbc->sync_mode == WB_SYNC_ALL && buffer_dirty((bh))) {
    sync_dirty_buffer(bh);

    if (buffer_req(bh) && !buffer_uptodate(bh)) {
      printk("IO error syncing inode [%s:%08lx]\n", inode->i_sb->s_id, inode->i_ino);
      ret = -EIO;
    }
  }

  brelse(bh);
  kfree(disk_inode);

out:
  return ret;
}

struct inode *bsfs_new_inode(const struct inode *dir, umode_t mode) {
  struct super_block   *sb    = dir->i_sb;
  struct minix_sb_info *sbi   = minix_sb(sb);
  struct inode         *inode = new_inode(sb);
  struct buffer_head   *bh;
  int                   bits_per_zone = 8 * sb->s_blocksize;
  unsigned long         j;
  int                   i;

  if (!inode)
    return ERR_PTR(-ENOMEM);
  j  = bits_per_zone;
  bh = NULL;
  spin_lock(&bitmap_lock);
  for (i = 0; i < sbi->s_imap_blocks; i++) {
    bh = sbi->s_imap[i];
    j  = minix_find_first_zero_bit(bh->b_data, bits_per_zone);
    if (j < bits_per_zone)
      break;
  }
  if (!bh || j >= bits_per_zone) {
    spin_unlock(&bitmap_lock);
    iput(inode);
    return ERR_PTR(-ENOSPC);
  }
  if (minix_test_and_set_bit(j, bh->b_data)) { /* shouldn't happen */
    spin_unlock(&bitmap_lock);
    printk("minix_new_inode: bit already set\n");
    iput(inode);
    return ERR_PTR(-ENOSPC);
  }
  spin_unlock(&bitmap_lock);
  mark_buffer_dirty(bh);
  j += i * bits_per_zone;
  if (!j || j > sbi->s_ninodes) {
    iput(inode);
    return ERR_PTR(-ENOSPC);
  }
  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
  inode->i_ino = j;
  simple_inode_init_ts(inode);
  inode->i_blocks = 0;
  memset(&minix_i(inode)->u, 0, sizeof(minix_i(inode)->u));
  insert_inode_hash(inode);
  mark_inode_dirty(inode);

  return inode;
}

void bsfs_free_inode(struct inode *inode) {
  kmem_cache_free(bsfs_inode_cache, bsfs_i(inode));
}

void bsfs_evict_inode(struct inode *inode) {
  truncate_inode_pages_final(&inode->i_data);

  if (!inode->i_nlink) {
    inode->i_size = 0;
    bsfs_truncate(inode);
  }

  invalidate_inode_buffers(inode);

  clear_inode(inode);

  if (!inode->i_nlink)
    bsfs_free_inode(inode);
}

static void init_once(void *foo) {
  struct bsfs_inode *ei = foo;
  inode_init_once(&ei->vfs_inode);
}

int __init init_inodecache(void) {
  bsfs_inode_cache = kmem_cache_create("bsfs_inode_cache", sizeof(struct bsfs_inode), 0, (SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT), init_once);

  if (bsfs_inode_cache == NULL)
    return -ENOMEM;

  return 0;
}

void destroy_inodecache(void) {
  /*
   * Make sure all delayed rcu free inodes are flushed before we
   * destroy cache.
   */
  rcu_barrier();
  kmem_cache_destroy(bsfs_inode_cache);
}
