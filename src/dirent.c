/**
 * @file dir_entry_operations_bsfs.c
 * @brief Directory entry B-tree operations for BSFS.
 *
 * Provides the in-memory directory descriptor (@ref dir_t) and the set of
 * operations for creating, removing, updating, looking up, and iterating
 * directory entries backed by a B-tree stored in the filesystem image.
 */
#include "../include/dirent.h"
#include "../include/btree.h"

/*
 * The tree is ordered by (hash(filename), filename) - see compare_dir_entry() -
 * so a lookup is a descent, not a scan. This used to walk every node of the
 * directory comparing names, which made each path resolution linear in the size
 * of the directory and also matched separator copies in internal nodes.
 */
int lookup_dir_entry(struct dentry *dentry, struct bsfs_dentry *out) {
  int ret;

  struct inode       *dir = d_inode(dentry->d_parent);
  struct super_block *sb  = dir->i_sb;

  struct bsfs_dentry needle = {0};
  btree_handle_t    *handle;

  if (dentry->d_name.len >= MAX_FILENAME)
    return -ENAMETOOLONG;

  handle = btree_create_dir_entry_handle();

  if (!handle)
    return -ENOMEM;

  handle->root_block = bsfs_i(dir)->btree_root;

  memcpy(needle.filename, dentry->d_name.name, dentry->d_name.len);
  needle.filename[dentry->d_name.len] = '\0';
  needle.name_hash                    = hash_filename(needle.filename);

  ret = btree_search_entry(sb, handle, handle->root_block, &needle, out);

  kfree(handle);

  return ret;
}

// TODO: is write_begin/write_end needed?
int bsfs_add_link(struct dentry *dentry, struct inode *inode) {
  int err = 0;

  struct inode *dir     = d_inode(dentry->d_parent);
  const char   *name    = dentry->d_name.name;
  int           namelen = dentry->d_name.len;

  struct super_block *sb = dir->i_sb;

  btree_handle_t *handle;

  struct bsfs_dentry entry  = {0};
  struct bsfs_dentry needle = {0};
  struct bsfs_dentry existing;

  if (namelen >= MAX_FILENAME)
    return -ENAMETOOLONG;

  handle = btree_create_dir_entry_handle();
  if (!handle)
    return -ENOMEM;

  handle->root_block = bsfs_i(dir)->btree_root;

  // strncpy(needle.filename, name, MAX_FILENAME - 1);
  // needle.filename[MAX_FILENAME - 1] = '\0';
  memcpy(needle.filename, name, namelen);
  needle.filename[namelen] = '\0';
  needle.name_hash         = hash_filename(needle.filename);

  // TODO: create btree_exist? or alike
  err = btree_search_entry(sb, handle, handle->root_block, &needle, &existing);

  if (err < 0)
    goto out;

  if (err == 1) {
    err = -EEXIST;
    goto out;
  }

  err = 0;

  entry.ino       = inode->i_ino;
  entry.name_hash = needle.name_hash;
  memcpy(entry.filename, needle.filename, namelen);
  needle.filename[namelen] = '\0';

  if (btree_insert(sb, handle, &entry) != 0) {
    err = -ENOSPC;
    goto out;
  }

  if (handle->root_block != bsfs_i(dir)->btree_root)
    bsfs_i(dir)->btree_root = handle->root_block;

  inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
  mark_inode_dirty(dir);

out:
  kfree(handle);
  return err;
}

int bsfs_remove_link(struct dentry *dentry) {
  struct inode       *dir    = d_inode(dentry->d_parent);
  struct super_block *sb     = dir->i_sb;
  btree_handle_t     *handle = NULL;
  struct bsfs_dentry  needle;
  int                 err    = 0;

  // The needle has to be a private copy: removal mutates the very buffers a
  // pointer into the tree would aim at (shift_left/merge/borrow, plus the
  // copy-on-write clones), so its contents would change mid-descent and the
  // tree would end up removing the neighbouring entry.
  err = lookup_dir_entry(dentry, &needle);

  if (err < 0)
    goto out;

  if (err != 1) {
    err = -ENOENT;
    goto out;
  }

  err    = 0;
  handle = btree_create_dir_entry_handle();

  if (!handle) {
    err = -ENOMEM;
    goto out;
  }

  // removing a name is a shrink, so it may spend the whole reserve - this is
  // one of the paths that has to keep working on a filesystem that is full
  handle->prio       = BSFS_ALLOC_SHRINK;
  handle->root_block = bsfs_i(dir)->btree_root;

  if (!btree_remove_entry(sb, handle, &needle)) {
    err = -EIO;
    goto out;
  }

  if (handle->root_block != bsfs_i(dir)->btree_root)
    bsfs_i(dir)->btree_root = handle->root_block;

  inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
  mark_inode_dirty(dir);

out:
  kfree(handle);
  return err;
}
