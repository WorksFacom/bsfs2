#include "../include/inode.h"
#include "../include/btree.h"
#include "../include/dir.h"
#include "../include/file.h"
#include "../include/rec_entry.h"
#include <linux/mpage.h>
#include <linux/version.h>
#include <linux/writeback.h>

// inode_state_read() only exists from 6.19 on; before that i_state is read directly.
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
#define inode_state_read(inode) ((inode)->i_state)
#endif

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

// TODO: sync once and use cached value on sbi until unmount?
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
  /* Persist inode root whenever it changes (e.g., after a split). */
  if (ret == 0 && handle->root_block != 0 && sbi->super->inode_root != handle->root_block) {
    sbi->super->inode_root = handle->root_block;
    mark_buffer_dirty(sbi->bh);
  }

  return ret;
}

// int update_inode(FILE *disk, btree_handle_t *handle, const bsfs_inode *inode) {
//   if (!handle || !inode)
//     return -1;
//   int ok = btree_update(disk, handle, handle->root_block, (void *)inode);
//   return ok;
// }

// int remove_inode(FILE *disk, bsfs_super_block *sb, btree_handle_t *handle, uint32_t inode_number) {
//   if (!handle || handle->type != BTREE_TYPE_INODE)
//     return -1;
//   int removed = btree_remove(disk, sb, handle, inode_number);
//   if (!removed && handle) {
//     bsfs_inode tmp;
//     int        present = btree_search(disk, handle, handle->root_block, inode_number, &tmp);
//     if (present) {
//       if (btree_remove_entry(disk, sb, handle, &tmp)) {
//         removed = 1;
//       }
//     }
//     if (!removed) {
//       fprintf(stderr, "remove_inode: failed to purge ino=%u (root=%u sb_root=%u present=%d)\n", inode_number, handle->root_block,
//               sb ? sb->inode_root : 0, present);
//     }
//   }
//   if (removed && sb->inode_root != handle->root_block) {
//     sb->inode_root = handle->root_block;
//     (void)write_superblock(disk, sb);
//   }
//   if (removed)
//     inode_sync_next(disk, sb, handle);
//   return removed;
// }

// int inode_exists(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number) {
//   bsfs_inode temp;
//   return btree_search(disk, inode_tree, inode_tree->root_block, inode_number, &temp);
// }

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

static void bsfs_read_bh_nowait(struct buffer_head *bh, blk_opf_t op_flags, bh_end_io_t *end_io) {
  BUG_ON(!buffer_locked(bh));

  if (bsfs_buffer_uptodate(bh)) {
    unlock_buffer(bh);
    return;
  }

  __bsfs_read_bh(bh, op_flags, end_io);
}

static int bsfs_read_bh(struct buffer_head *bh, blk_opf_t op_flags, bh_end_io_t *end_io) {
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

static int __maybe_unused bsfs_read_bh_lock(struct buffer_head *bh, blk_opf_t op_flags, bool wait) {
  lock_buffer(bh);
  if (!wait) {
    bsfs_read_bh_nowait(bh, op_flags, NULL);
    return 0;
  }
  return bsfs_read_bh(bh, op_flags, NULL);
}

// TODO: test
// int bsfs_block_write_begin(struct folio *folio, loff_t pos, unsigned len, get_block_t *get_block) {
//   unsigned int        from  = offset_in_folio(folio, pos);
//   unsigned            to    = from + len;
//   struct inode       *inode = folio->mapping->host;
//   unsigned            block_start, block_end;
//   sector_t            block;
//   int                 err       = 0;
//   unsigned            blocksize = inode->i_sb->s_blocksize;
//   unsigned            bbits;
//   struct buffer_head *bh, *head, *wait[2];
//   int                 nr_wait = 0;
//   int                 i;
//
//   BUG_ON(!folio_test_locked(folio));
//   BUG_ON(to > folio_size(folio));
//   BUG_ON(from > to);
//
//   head = folio_buffers(folio);
//   if (!head)
//     head = create_empty_buffers(folio, blocksize, 0);
//   bbits = ilog2(blocksize);
//   block = (sector_t)folio->index << (PAGE_SHIFT - bbits);
//
//   for (bh = head, block_start = 0; bh != head || !block_start; block++, block_start = block_end, bh = bh->b_this_page) {
//     block_end = block_start + blocksize;
//     if (block_end <= from || block_start >= to) {
//       if (folio_test_uptodate(folio)) {
//         set_buffer_uptodate(bh);
//       }
//       continue;
//     }
//     if (WARN_ON_ONCE(buffer_new(bh)))
//       clear_buffer_new(bh);
//     if (!buffer_mapped(bh)) {
//       WARN_ON(bh->b_size != blocksize);
//
//       err = get_block(inode, block, bh, 1);
//
//       if (err)
//         break;
//
//       if (buffer_new(bh)) {
//         if (folio_test_uptodate(folio)) {
//           /*
//            * Unlike __block_write_begin() we leave
//            * dirtying of new uptodate buffers to
//            * ->write_end() time or
//            * folio_zero_new_buffers().
//            */
//           set_buffer_uptodate(bh);
//           continue;
//         }
//         if (block_end > to || block_start < from)
//           folio_zero_segments(folio, to, block_end, block_start, from);
//         continue;
//       }
//     }
//     if (folio_test_uptodate(folio)) {
//       set_buffer_uptodate(bh);
//       continue;
//     }
//     if (!buffer_uptodate(bh) && !buffer_delay(bh) && !buffer_unwritten(bh) && (block_start < from || block_end > to)) {
//       bsfs_read_bh_lock(bh, 0, false);
//       wait[nr_wait++] = bh;
//     }
//   }
//   /*
//    * If we issued read requests, let them complete.
//    */
//   for (i = 0; i < nr_wait; i++) {
//     wait_on_buffer(wait[i]);
//     if (!buffer_uptodate(wait[i]))
//       err = -EIO;
//   }
//
//   if (unlikely(err)) {
//     folio_zero_new_buffers(folio, from, to);
//   }
//
//   return err;
// }

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

  if (!bspan_h)
    return;

  // taken after block_truncate_page: it calls bsfs_get_block, which takes this mutex itself
  mutex_lock(&bsfs_inode->truncate_mutex);

  bspan_h->root_block = bsfs_inode->btree_root;

  // 1) Shrink overlapping head span if needed
  bspan_t span;
  int     found = bspan_search_le(sb, bspan_h, iblock, &span);

  if (found < 0)
    pr_warn("truncate: bspan lookup failed (%d), skipping head-span shrink\n", found);

  if (found == 1) {
    if (iblock < span.file_blk + span.length) {
      // free tail portion beyond new_size
      bspan_t tail     = span;
      __u32   keep_len = (iblock > span.file_blk) ? (iblock - span.file_blk) : 0;
      tail.file_blk    = iblock;
      tail.disk_blk    = span.disk_blk + keep_len;
      tail.length      = span.length - keep_len;

      // shrink the span in the tree first
      // only deallocate the tail blocks once the tree no longer references them
      span.length = keep_len;
      if (keep_len > 0) {
        if (btree_update(sb, bspan_h, bspan_h->root_block, &span) == 1)
          deallocate_bspan(sb, &tail);
        else
          pr_warn("truncate: failed to shrink span at file_blk %u, leaking %u blocks\n", span.file_blk, tail.length);
      } else {
        // remove empty span entry
        if (btree_remove(sb, bspan_h, span.file_blk))
          deallocate_bspan(sb, &tail);
        else
          pr_warn("truncate: failed to remove span at file_blk %u, leaking %u blocks\n", span.file_blk, tail.length);
      }
    }
  }

  // 2) Collect and remove all spans starting at or after iblock
  // Traverse tree and collect keys >= threshold
  typedef struct {
    __u32 k;
  } krec;

  // TODO: large stack/frame size
  // krec   keys[1024];
  krec   keys[256];
  size_t kcount = 0;
  // simple DFS stack of node blocks
  __u32 stack[128];
  int   top = 0;

  if (bspan_h->root_block)
    stack[top++] = bspan_h->root_block;

  while (top > 0) {
    int   ret;
    __u32 nb = stack[--top];

    ret = btree_read_node(sb, bspan_h, nb, &node);

    if (ret != 0)
      continue;

    // push children first for post-order like
    if (!node->node->is_leaf) {
      for (__u32 i = 0; i <= node->node->num_entries; i++) {
        if (top < (int)(sizeof(stack) / sizeof(stack[0])))
          stack[top++] = bchildren(node, bspan_h)[i];
      }
    }

    // B+: only leaves hold records; entries in internal nodes are separator
    // copies, so collecting them would just queue the same key twice.
    if (node->node->is_leaf) {
      for (__u32 i = 0; i < node->node->num_entries; i++) {
        __u32 ekey = bspan_h->get_key(bentries(node) + i * bspan_h->entry_size);
        if (ekey >= iblock) {
          if (kcount < sizeof(keys) / sizeof(keys[0])) {
            keys[kcount++].k = ekey;
          }
        }
      }
    }

    btree_free_node(node);
  }

  // Remove collected keys
  for (size_t i = 0; i < kcount; i++) {
    bspan_t *found;
    bspan_t  bs;

    // btree_search returns a pointer into the node's buffer
    // copy the span out before releasing the node
    if (btree_search(sb, bspan_h, bspan_h->root_block, keys[i].k, (void **)&found, &node) == 1) {
      bs = *found;
      btree_free_node(node);
      // drop the tree reference first, then free the blocks
      if (btree_remove(sb, bspan_h, keys[i].k))
        deallocate_bspan(sb, &bs);
      else
        pr_warn("truncate: failed to remove span at file_blk %u, leaking %u blocks\n", bs.file_blk, bs.length);
    }
  }

  // removals may have shrunk (or emptied) the tree root
  if (bsfs_inode->btree_root != bspan_h->root_block) {
    bsfs_inode->btree_root = bspan_h->root_block;
    mark_inode_dirty(inode);
  }

  mutex_unlock(&bsfs_inode->truncate_mutex);
  kfree(bspan_h);
}

// old read_inode() on btree.c
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

  // TODO: applicable?
  // if (!ino || ino > sbi->s_ninodes) {
  //   printk("Bad inode number on dev %s: %ld is out of range\n", sb->s_id, (long)ino);
  //   return NULL;
  // }

  // free btree_node without releasing buffer
  // buffer will be brelsed later
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
 * Writes the in-core inode back into the inode tree.
 *
 * This goes through btree_update() rather than writing through the pointer
 * bsfs_read_disk_inode() returns: that pointer aims into a tree node's buffer,
 * and under copy-on-write the next mutation clones that node and releases the
 * old block, so the write would land on a block the allocator has already
 * handed to somebody else.
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

  mutex_lock(&sbi->inode_tree_lock);

  inode_h->root_block = sbi->super->inode_root;

  // Take a private copy of the entry: the fields the VFS does not own (block
  // count, padding) have to survive the update.
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

  // TODO: make it work with special files
  // if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
  //   raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);

  if (btree_update(sb, inode_h, inode_h->root_block, &entry) != 1) {
    ret = -EIO;
    goto out;
  }

  // btree_update() publishes a new root on every call, so the superblock has to
  // follow it.
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
    printk(KERN_DEBUG "BSFS: Invalid file type 0%04o for inode %lu.\n", inode->i_mode, inode->i_ino);
    make_bad_inode(inode);
  }
}

static struct inode *__set_up_inode_new(struct inode *inode) {
  struct disk_mapped_inode *disk_inode;
  struct bsfs_super_info   *sbi = bsfs_sb(inode->i_sb);

  // hold the tree lock while reading through disk_inode->inode (a pointer
  // into the tree node's buffer)
  mutex_lock(&sbi->inode_tree_lock);

  disk_inode = bsfs_read_disk_inode(inode->i_sb, inode->i_ino);

  struct bsfs_inode *bsfs_inode = bsfs_i(inode);

  if (!disk_inode) {
    mutex_unlock(&sbi->inode_tree_lock);
    iget_failed(inode);
    return ERR_PTR(-EIO);
  }

  if (disk_inode->inode->nlinks == 0) {
    printk("BSFS: deleted inode referenced: %lu\n", inode->i_ino);
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

  // TODO: whats this?
  inode->i_blocks = 0;

  bsfs_inode->btree_root = disk_inode->inode->btree_root;

  // TODO: support special file inodes
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

  // initialized once per slab object lifetime, not on every reuse
  bsfs_inode = alloc_inode_sb(sb, bsfs_inode_cache, GFP_KERNEL);

  if (!bsfs_inode)
    return NULL;

  // no pending recovery snapshot until bsfs_unlink stashes one
  // the slab is recycled, so clear the sentinel explicitly
  bsfs_inode->i_deleted_parent_ino = 0;

  return &bsfs_inode->vfs_inode;
}

int bsfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
  struct bsfs_super_info *sbi = bsfs_sb(inode->i_sb);
  int                     ret;

  ret = bsfs_update_inode(inode);

  if (ret != 0)
    return ret;

  // The entry now lives in whichever node the copy-on-write update produced, so
  // there is no single buffer to sync here; what has to be durable is the root
  // that points at it. The node buffers themselves are flushed by bsfs_fsync()'s
  // sync_blockdev() and by regular bdev writeback.
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

  // covers ino assignment + insert so two creators can't race to the same
  // ino or mutate the inode tree concurrently
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

  struct super_block     *sb  = inode->i_sb;
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  struct bsfs_inode      *bi  = bsfs_i(inode);

  truncate_inode_pages_final(&inode->i_data);

  if (want_delete) {
    // other FS would do inode->i_size = 0 then truncate(inode)
    // we don't do that here since we'll add a rec entry and need the data to still be on-disk

    // build the recovery snapshot from the inode's final state, before we tear it down
    // a non-zero stashed parent ino means this inode reached nlink 0 via bsfs_unlink (vs. a bad inode or other path), so it should be
    // recoverable
    if (bi->i_deleted_parent_ino) {
      btree_handle_t *rec_tree = btree_create_rec_entry_handle();

      if (rec_tree) {
        rec_entry_t rec = {0};

        // recovery_id is assigned by bsfs_rec_tree_add
        rec.deleted_at          = ktime_get_real_seconds();
        rec.original_parent_ino = bi->i_deleted_parent_ino;

        strscpy(rec.original_name, bi->i_deleted_name, sizeof(rec.original_name));

        rec.inode_number    = inode->i_ino;
        rec.file_type       = S_ISDIR(inode->i_mode) ? 0 : 1;
        rec.uid             = fs_high2lowuid(i_uid_read(inode));
        rec.gid             = fs_high2lowgid(i_gid_read(inode));
        rec.permissions     = inode->i_mode;
        rec.file_size       = inode->i_size;
        rec.block_count     = 0; // not maintained on disk, reclaim walks btree_root
        rec.btree_root      = bi->btree_root;
        rec.retention_until = rec.deleted_at + DEFAULT_RECOVERY_TTL;

        rec_tree->root_block = sbi->super->recovery_root;

        mutex_lock(&sbi->rec_lock);

        // bsfs_rec_tree_add persists recovery_root to the super block itself
        if (bsfs_rec_tree_add(sb, rec_tree, &rec))
          pr_warn("failed to record recovery snapshot for inode %lu\n", inode->i_ino);

        mutex_unlock(&sbi->rec_lock);

        kfree(rec_tree);
      } else {
        pr_warn("no memory for recovery snapshot of inode %lu\n", inode->i_ino);
      }
    }
  }

  invalidate_inode_buffers(inode);
  clear_inode(inode);

  if (want_delete) {
    btree_handle_t *inode_h = btree_create_inode_handle();

    if (inode_h) {
      mutex_lock(&sbi->inode_tree_lock);

      inode_h->root_block = sbi->super->inode_root;
      btree_remove(sb, inode_h, inode->i_ino);
      if (sbi->super->inode_root != inode_h->root_block) {
        sbi->super->inode_root = inode_h->root_block;
        mark_buffer_dirty(sbi->bh);
      }

      mutex_unlock(&sbi->inode_tree_lock);
      kfree(inode_h);
    } else {
      pr_warn("no memory to remove inode %lu from inode tree\n", inode->i_ino);
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
  /*
   * Make sure all delayed rcu free inodes are flushed before we
   * destroy cache.
   */
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

  // don't fall through to allocation on a failed lookup
  // the block may already be mapped, a second allocation would shadow it
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

  // falling through to the allocation path, since nothing was inserted into the bspan tree during write_begin (deliberately)
  // alloc physical block, insert new bspan extent, map_bh then overwrites the dummy b_blocknr=0 with the real disk blocknr

  __u32 phys_block = allocate_single_block(sb);

  if (phys_block == INVALID_BLOCK) {
    ret = -ENOSPC;
    goto out;
  }

  //  try to extend predecessor span if physically contiguous
  if (found == 1 && iblock == span.file_blk + span.length && span.disk_blk + span.length == phys_block) {
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

// Marks holes as buffer_delay without physical allocation.
//
// CURRENTLY UNUSED: mpage_writepages() knows nothing about BH_Delay - it only
// looks at buffer_mapped(), so a buffer marked here with map_bh(bh, sb, 0) was
// taken to be "already allocated" and writeback wrote the data to block 0 (the
// reserved one), never calling bsfs_get_block() nor creating the bspan. Using
// delayed allocation would require our own ->writepages, ext4-style
// (mpage_da_*), deciding the allocation at flush time. Until then allocation
// happens in write_begin.
static int __maybe_unused bsfs_get_block_delalloc(struct inode *inode, sector_t iblock, struct buffer_head *bh, int create) {
  int found;
  int ret = 0;

  struct super_block *sb = inode->i_sb;

  struct bsfs_inode *bsfs_inode = bsfs_i(inode);
  bspan_t            span;

  btree_handle_t *bspan_h = btree_create_bspan_handle();
  if (!bspan_h) {
    return -ENOMEM;
  }

  mutex_lock(&bsfs_inode->truncate_mutex);

  bspan_h->root_block = bsfs_inode->btree_root;

  found = bspan_search_le(sb, bspan_h, iblock, &span);

  if (found < 0) {
    ret = found;
    goto out;
  }

  // explicit bounds check
  // bspan_search_le should be correct but we're being defensive
  if (found == 1 && iblock >= span.file_blk && iblock < span.file_blk + span.length) {
    map_bh(bh, sb, span.disk_blk + (iblock - span.file_blk));
    goto out;
  }

  // should never happen!
  if (!create)
    goto out;

  // hole: mark as delayed allocation, no physical block assigned yet
  map_bh(bh, sb, 0);
  set_buffer_new(bh);
  set_buffer_delay(bh);

out:
  mutex_unlock(&bsfs_inode->truncate_mutex);
  kfree(bspan_h);
  return ret;
}

static void bsfs_write_failed(struct address_space *mapping, loff_t to) {
  struct inode *inode = mapping->host;

  if (to > inode->i_size) {
    truncate_pagecache(inode, inode->i_size);
    bsfs_truncate(inode);
  }
}

static int bsfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop,
                            void **fsdata) {
  int ret;

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

    // fully past eof: racing truncate, nothing to write
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
