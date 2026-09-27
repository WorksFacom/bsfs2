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

    // B+: only the rightmost leaf holds the largest record. An internal node's
    // last entry is a separator, i.e. a copy of some leaf's *first* entry, so
    // falling back to it here would hand out an inode number that is already in
    // use. A broken child pointer means we cannot answer.
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
//       bh_read_nowait(bh, 0);
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

  // this is the path that gives space back, so it may spend the whole reserve
  bspan_h->prio = BSFS_ALLOC_SHRINK;

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

      // shrink the span in the tree first, then queue the tail: the new extent
      // root is still only in the dirty inode, so until it is written back the
      // on-disk tree can still reach these blocks
      span.length = keep_len;
      if (keep_len > 0) {
        if (btree_update(sb, bspan_h, bspan_h->root_block, &span) == 1)
          bsfs_defer_free_bspan(sb, &tail);
        else
          pr_warn("truncate: failed to shrink span at file_blk %u, leaking %u blocks\n", span.file_blk, tail.length);
      } else {
        // remove empty span entry
        if (btree_remove(sb, bspan_h, span.file_blk))
          bsfs_defer_free_bspan(sb, &tail);
        else
          pr_warn("truncate: failed to remove span at file_blk %u, leaking %u blocks\n", span.file_blk, tail.length);
      }
    }
  }

  // 2) Collect and remove every span starting at or after iblock.
  //
  // The collect buffer is bounded (it lives on the kernel stack), so a heavily
  // fragmented file can hold more spans than fit in a single pass - it used to
  // silently drop the excess and truncate only part of the file. Sweep instead:
  // collect a batch, remove it, and go again until a pass finds nothing left
  // above the cut.
  __u32  keys[64];
  size_t kcount;

  // DFS over the tree. bspan entries are the smallest of all the trees here
  // (12 bytes), so their nodes have the widest fan-out: 2t = 256 children. The
  // stack holds a full sibling set; if it ever fills anyway, the sweep above
  // just runs another pass.
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

      // B+: only leaves hold records; entries in internal nodes are separator
      // copies, so collecting them would just queue the same key twice.
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

      // btree_search returns a pointer into the node's buffer
      // copy the span out before releasing the node
      if (btree_search(sb, bspan_h, bspan_h->root_block, keys[i], (void **)&found, &node) == 1) {
        bs = *found;
        btree_free_node(node);
        // drop the tree reference first, then queue the blocks behind the same
        // barrier the metadata uses
        if (btree_remove(sb, bspan_h, keys[i])) {
          bsfs_defer_free_bspan(sb, &bs);
          removed++;
        } else {
          pr_warn("truncate: failed to remove span at file_blk %u, leaking %u blocks\n", bs.file_blk, bs.length);
        }
      }
    }

    // A pass that removes nothing would sweep forever.
    if (removed == 0) {
      pr_warn("truncate: %zu spans past the cut could not be removed\n", kcount);
      break;
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

  // Persisting an inode is what lets the deferred blocks be released, so it
  // outranks ordinary growth - but not the shrinking paths, which are the ones
  // that must still work on a filesystem that is genuinely full.
  inode_h->prio = BSFS_ALLOC_WRITEBACK;

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
    printk(KERN_DEBUG "BSFS: Invalid file type 0%04o for inode %llu.\n", inode->i_mode, inode->i_ino);
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

  if (ret != 0) {
    // This inode's tree root is still only in memory, so the on-disk tree is
    // the old one and the blocks it replaced must not be recycled. The commit
    // path checks this before releasing anything.
    mutex_lock(&sbi->pending_free_lock);
    // a hard error outranks a stored ENOSPC, so the commit reports the worst thing that happened
    if (!sbi->writeback_error || (sbi->writeback_error == -ENOSPC && ret != -ENOSPC))
      sbi->writeback_error = ret;
    mutex_unlock(&sbi->pending_free_lock);

    return ret;
  }

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
  bool snapshot    = false;
  // btree_root means a dentry tree on a directory and an extent tree on a file;
  // read it before clear_inode() so the reclaim below picks the right handle
  bool is_dir      = S_ISDIR(inode->i_mode);

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
          pr_warn("failed to record recovery snapshot for inode %llu\n", inode->i_ino);
        else
          snapshot = true; // the snapshot owns the data blocks now

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

    // Nobody took a recovery snapshot, so this inode's own tree - and, for a
    // file, every block it maps - is now unreachable. Without this they stayed
    // allocated until the next mkfs: the only code that ever freed them lives
    // in the recovery GC, which is still out of the build.
    if (!snapshot && bi->btree_root) {
      // the handle decides how the entries are read, so a dentry tree must not
      // be walked as a tree of spans
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

  // This block was very likely a btree node a moment ago - copy-on-write and
  // the data allocator trade the same blocks back and forth. Its buffer is
  // still sitting dirty in the bdev mapping, and file data goes through the
  // inode's mapping instead, so without this the two aliases race to the same
  // disk block and whichever writeback runs last wins.
  clean_bdev_aliases(sb->s_bdev, phys_block, 1);

  //  try to extend predecessor span if physically contiguous
  //  bspan_t.length is a __u16, so a span has to stop growing well before it
  //  wraps - at 0 the mapping check below silently stops matching and we would
  //  insert a second span under the same file_blk
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
 * Allocation happens here, not at writeback time. Delayed allocation was tried
 * (a get_block variant that only marked holes with buffer_delay) and does not
 * work with mpage_writepages(): it knows nothing about BH_Delay, so a buffer
 * marked with map_bh(bh, sb, 0) looked "already allocated" and writeback wrote
 * the data to block 0, never calling back to allocate or to create the bspan.
 * Doing it for real needs our own ->writepages, ext4-style (mpage_da_*), which
 * decides the allocation at flush time.
 */
static int bsfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop,
                            void **fsdata) {
  int ret;

  // cheap check, and it keeps the common case from ever reaching the retry
  if (bsfs_should_reclaim(mapping->host->i_sb))
    (void)bsfs_reclaim_space(mapping->host->i_sb);

  ret = block_write_begin(mapping, pos, len, foliop, bsfs_get_block);

  // Copy-on-write parks the blocks it replaces until a commit point, so ENOSPC
  // here may only mean the deferred list is holding them. block_write_begin()
  // already dropped the folio on error and we hold no tree lock, so this is the
  // one spot that can run the barrier and try again.
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
