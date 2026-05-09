static inline void truncate(struct inode *inode) {
  struct super_block *sb    = inode->i_sb;
  block_t            *idata = i_data(inode);
  int                 offsets[DEPTH];
  Indirect            chain[DEPTH];
  Indirect           *partial;
  block_t             nr = 0;
  int                 n;
  int                 first_whole;
  long                iblock;

  iblock = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
  block_truncate_page(inode->i_mapping, inode->i_size, get_block);

  n = block_to_path(inode, iblock, offsets);
  if (!n)
    return;

  if (n == 1) {
    free_data(inode, idata + offsets[0], idata + DIRECT);
    first_whole = 0;
    goto do_indirects;
  }

  first_whole = offsets[0] + 1 - DIRECT;
  partial     = find_shared(inode, n, offsets, chain, &nr);
  if (nr) {
    if (partial == chain)
      mark_inode_dirty(inode);
    else
      mark_buffer_dirty_inode(partial->bh, inode);
    free_branches(inode, &nr, &nr + 1, (chain + n - 1) - partial);
  }
  /* Clear the ends of indirect blocks on the shared branch */
  while (partial > chain) {
    free_branches(inode, partial->p + 1, block_end(partial->bh), (chain + n - 1) - partial);
    mark_buffer_dirty_inode(partial->bh, inode);
    brelse(partial->bh);
    partial--;
  }
do_indirects:
  /* Kill the remaining (whole) subtrees */
  while (first_whole < DEPTH - 1) {
    nr = idata[DIRECT + first_whole];
    if (nr) {
      idata[DIRECT + first_whole] = 0;
      mark_inode_dirty(inode);
      free_branches(inode, &nr, &nr + 1, first_whole + 1);
    }
    first_whole++;
  }
  inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
  mark_inode_dirty(inode);
}

static inline unsigned nblocks(loff_t size, struct super_block *sb) {
  int      k = sb->s_blocksize_bits - 10;
  unsigned blocks, res, direct = DIRECT, i = DEPTH;
  blocks = (size + sb->s_blocksize - 1) >> (BLOCK_SIZE_BITS + k);
  res    = blocks;
  while (--i && blocks > direct) {
    blocks -= direct;
    blocks += sb->s_blocksize / sizeof(block_t) - 1;
    blocks /= sb->s_blocksize / sizeof(block_t);
    res += blocks;
    direct = 1;
  }
  return res;
}
