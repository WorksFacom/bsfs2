#include "../bsfs.h"

static __u32 s_next_inode_number = 1;
static __u32 s_cached_inode_root = INVALID_BLOCK;

static int inode_tree_find_max(FILE *disk, btree_handle_t *handle, uint32_t node_block, bsfs_inode *out_inode) {
  if (!handle || node_block == 0 || !out_inode)
    return 0;

  btree_node_t *node = btree_read_node(disk, handle, node_block);
  if (!node)
    return 0;

  int ok = 0;
  if (node->is_leaf) {
    if (node->num_entries > 0) {
      memcpy(out_inode, (uint8_t *)node->entries + (node->num_entries - 1) * handle->entry_size, handle->entry_size);
      ok = 1;
    }
  } else {
    uint32_t child_index = node->num_entries;
    if (child_index >= (uint32_t)(2 * handle->min_degree)) {
      child_index = (node->num_entries > 0) ? node->num_entries - 1 : 0;
    }
    uint32_t child_block = node->children[child_index];
    if (child_block != 0 && child_block != INVALID_BLOCK)
      ok = inode_tree_find_max(disk, handle, child_block, out_inode);
    if (!ok && node->num_entries > 0) {
      memcpy(out_inode, (uint8_t *)node->entries + (node->num_entries - 1) * handle->entry_size, handle->entry_size);
      ok = 1;
    }
  }

  btree_free_node(node);
  return ok;
}

static void inode_sync_next(FILE *disk, bsfs_super_block *sb, btree_handle_t *inode_tree) {
  uint32_t root_block = inode_tree ? inode_tree->root_block : 0;
  if (sb)
    root_block = sb->inode_root;

  if (root_block == s_cached_inode_root)
    return;

  if (root_block == 0 || !inode_tree) {
    s_next_inode_number = 1;
    s_cached_inode_root = root_block;
    return;
  }

  bsfs_inode max_inode;
  if (inode_tree_find_max(disk, inode_tree, root_block, &max_inode)) {
    s_next_inode_number = max_inode.inode_number + 1;
  } else {
    s_next_inode_number = 1;
  }
  s_cached_inode_root = root_block;
}

// TODO: update all functions using read_inode to use bsfs_iget
int read_inode(FILE *disk, btree_handle_t *handle, __u32 inode_number, bsfs_inode *out_inode) {
  if (!handle || !out_inode)
    return 0;

  return btree_search(disk, handle, handle->root_block, inode_number, out_inode);
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

uint32_t get_next_inode_number(FILE *disk, bsfs_super_block *sb, btree_handle_t *inode_tree) {
  if (!sb)
    return s_next_inode_number++;

  inode_sync_next(disk, sb, inode_tree);

  if (s_next_inode_number == 0)
    s_next_inode_number = 1;

  return s_next_inode_number++;
}

int inode_exists(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number) {
  bsfs_inode temp;
  return btree_search(disk, inode_tree, inode_tree->root_block, inode_number, &temp);
}

int inode_tree_init(FILE *disk, bsfs_super_block *sb, btree_handle_t *out) {
  if (!disk || !sb || !out)
    return 0;

  btree_handle_t *tmp = btree_create_inode_handle();
  if (!tmp)
    return 0;

  if (sb->inode_root == 0) {
    // Create empty root leaf node on disk
    btree_node_t node;
    node.is_leaf     = 1;
    node.num_entries = 0;
    node.entries     = calloc((size_t)(2 * tmp->min_degree - 1), (size_t)tmp->entry_size);
    node.children    = calloc((size_t)(2 * tmp->min_degree), sizeof(uint32_t));
    if (!node.entries || !node.children) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }

    uint32_t root_block = allocate_single_block(disk, sb);
    if (root_block == INVALID_BLOCK || root_block == 0) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }

    tmp->root_block = root_block;
    if (btree_write_node(disk, tmp, root_block, &node) != 0) {
      free(node.entries);
      free(node.children);
      free(tmp);
      return 0;
    }
    free(node.entries);
    free(node.children);

    sb->inode_root = root_block;
    if (write_superblock(disk, sb) != 0) {
      free(tmp);
      return 0;
    }
  } else {
    tmp->root_block = sb->inode_root;
  }

  *out = *tmp;
  free(tmp);
  return 1;
}

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

static int bsfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop,
                            void **fsdata) {
  int ret, retries = 0;

  struct folio *folio;
  pgoff_t       index;
  struct inode *inode = mapping->host;

  index = pos >> PAGE_SHIFT;

  *fsdata = (void *)0;

  folio = write_begin_get_folio(iocb, mapping, index, len);
  if (IS_ERR(folio))
    return PTR_ERR(folio);

  if (pos + len > folio_pos(folio) + folio_size(folio))
    len = folio_pos(folio) + folio_size(folio) - pos;

  ret = bsfs_block_write_begin(folio, pos, len, ext4_da_get_block_prep);
  if (ret < 0) {
    folio_unlock(folio);
    folio_put(folio);
    /*
     * ext4_block_write_begin may have instantiated a few blocks
     * outside i_size.  Trim these off again. Don't need
     * i_size_read because we hold inode lock.
     */
    if (pos + len > inode->i_size)
      ext4_truncate_failed_write(inode);

    return ret;
  }

  *foliop = folio;
  return ret;
}

static const struct address_space_operations bsfs_aops = {
    .write_begin = bsfs_write_begin
    // .write_end = generic_write_end,
    //  .writepages = bsfs_writepages,
};
