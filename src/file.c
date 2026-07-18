/**
 * @file file_operations_bsfs.c
 * @brief Implementation of file-level operations in the BSFS file system.
 *
 * This file provides functions to manipulate files, including opening,
 * closing, reading, writing, and seeking, by utilizing inodes and
 * B-tree-based address mapping. Files are loaded into memory as `file_t`
 * structures which manage stateful access.
 */
#include "../include/file.h"
#include "../include/inode.h"

#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/writeback.h>

// generic_file_fsync won't cut it.
// btree nodes, the bitmap and the superblock are dirtied with mark_buffer_dirty.
// they live in the bdev mapping rather than the inode's private buffer list.
// sync_mapping_buffers never sees them
// see `bsfs_write_inode`
int bsfs_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
  int ret, err;

  struct inode       *inode = file->f_mapping->host;
  struct super_block *sb    = inode->i_sb;

  // data pages first. writeback allocates delalloc blocks, which dirties btree node BHs, the bitmap and the inode
  ret = file_write_and_wait_range(file, start, end);

  // -> eventually calls `bsfs_write_inode` with WB_SYNC_ALL
  err = sync_inode_metadata(inode, 1);

  if (!ret)
    ret = err;

  err = sync_blockdev(sb->s_bdev);
  if (!ret)
    ret = err;

  if (!ret)
    ret = blkdev_issue_flush(sb->s_bdev);

  return ret;
}

const struct file_operations bsfs_file_operations = {
    .read_iter    = generic_file_read_iter,
    .write_iter   = generic_file_write_iter,
    .llseek       = generic_file_llseek,
    .mmap         = generic_file_mmap,
    .splice_read  = filemap_splice_read,
    .splice_write = iter_file_splice_write,
    .fsync        = bsfs_fsync,
};

// VFS default truncate only updates i_size and the page cache. The on-disk extents
// past the cut point survive, leak their blocks and read back as stale data once
// the file is extended again. bsfs_truncate works off i_size, so set it first.
static int bsfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr) {
  struct inode *inode = d_inode(dentry);
  int           error;

  error = setattr_prepare(idmap, dentry, attr);
  if (error)
    return error;

  if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != i_size_read(inode)) {
    inode_dio_wait(inode);
    truncate_setsize(inode, attr->ia_size);
    bsfs_truncate(inode);
    inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
  }

  setattr_copy(idmap, inode, attr);
  mark_inode_dirty(inode);
  return 0;
}

const struct inode_operations bsfs_file_inode_operations = {
    .setattr = bsfs_setattr,
};

const struct inode_operations bsfs_symlink_inode_operations = {

};
