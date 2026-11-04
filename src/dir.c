/**
 * @file dir.c
 * @brief directory inode and file operations.
 */

#include "../include/dir.h"
#include "../bsfs.h"
#include "../include/btree.h"
#include "../include/dirent.h"
#include "../include/file.h"
#include "../include/inode.h"
#include "../include/rec_entry.h"

ino_t bsfs_inode_by_name(struct dentry *dentry) {
  struct bsfs_dentry dirent;

  if (lookup_dir_entry(dentry, &dirent) != 1)
    return 0;

  return dirent.ino;
}

static struct dentry *bsfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
  ino_t         ino;
  struct inode *inode = NULL;

  if (dentry->d_name.len >= MAX_FILENAME)
    return ERR_PTR(-ENAMETOOLONG);

  ino = bsfs_inode_by_name(dentry);

  if (ino)
    inode = bsfs_get_vfs_inode(dir->i_sb, ino);

  return d_splice_alias(inode, dentry);
}

/*
 * every op below mutates at least two trees, so commit here with no tree lock held instead of
 * running dry halfway through a removal.
 */
static void bsfs_reclaim_if_low(struct inode *dir) {
  if (bsfs_should_reclaim(dir->i_sb))
    (void)bsfs_reclaim_space(dir->i_sb);
}

static int link_nondir(struct dentry *dentry, struct inode *inode) {
  int err = bsfs_add_link(dentry, inode);

  if (!err) {
    d_instantiate(dentry, inode);
    return 0;
  }

  inode_dec_link_count(inode);
  iput(inode);
  return err;
}

static int __bsfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t rdev) {
  struct inode *inode;

  inode = bsfs_new_inode(dir, mode);

  if (IS_ERR(inode))
    return PTR_ERR(inode);

  bsfs_set_inode(inode, rdev);

  mark_inode_dirty(inode);

  return link_nondir(dentry, inode);
}

/*
 * ENOSPC here often means the deferred list holds every free block. no tree lock is held, so run
 * the barrier and retry. failure leaves the dentry negative, so the retry starts clean.
 */
static int bsfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t rdev) {
  int err;

  // TODO: old_valid_dev or new_valid_dev?
  if (!old_valid_dev(rdev))
    return -EINVAL;

  bsfs_reclaim_if_low(dir);

  err = __bsfs_mknod(dir, dentry, mode, rdev);

  if (err == -ENOSPC && bsfs_reclaim_space(dir->i_sb) == 0)
    err = __bsfs_mknod(dir, dentry, mode, rdev);

  return err;
}

static int bsfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
  return bsfs_mknod(&nop_mnt_idmap, dir, dentry, mode, 0);
}

static int __bsfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
  int err;

  struct inode *inode;

  inode = bsfs_new_inode(dir, S_IFDIR | mode);

  if (IS_ERR(inode))
    return PTR_ERR(inode);

  inode_inc_link_count(dir); // new ".."
  bsfs_set_inode(inode, 0);
  inode_inc_link_count(inode); // "." makes it 2

  mark_inode_dirty(inode);

  // . and .. are synthesized, only mkfs stores them, in the root
  err = bsfs_add_link(dentry, inode);

  if (err)
    goto out_fail;

  d_instantiate(dentry, inode);
out:
  return err;

out_fail:
  inode_dec_link_count(inode);
  inode_dec_link_count(inode);
  iput(inode);
  inode_dec_link_count(dir);
  goto out;
}

// same ENOSPC retry as bsfs_mknod
static struct dentry *bsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
  int err;

  bsfs_reclaim_if_low(dir);

  err = __bsfs_mkdir(dir, dentry, mode);

  if (err == -ENOSPC && bsfs_reclaim_space(dir->i_sb) == 0)
    err = __bsfs_mkdir(dir, dentry, mode);

  return ERR_PTR(err);
}

// no snapshot here, the file may still be open or have other links. evict builds it at i_nlink == 0
static int bsfs_unlink(struct inode *dir, struct dentry *dentry) {
  int err;

  struct inode *inode = d_inode(dentry);

  if (S_ISDIR(inode->i_mode))
    return -EISDIR;

  bsfs_reclaim_if_low(dir);

  bsfs_i(inode)->i_deleted_parent_ino = dir->i_ino;
  strscpy(bsfs_i(inode)->i_deleted_name, dentry->d_name.name, MAX_FILENAME);

  err = bsfs_remove_link(dentry);
  if (err)
    return err;

  inode_dec_link_count(inode);

  return 0;
}

// . and .. are only on disk in the root, which can't be removed, so any entry means not empty
static bool bsfs_subtree_has_entry(struct super_block *sb, btree_handle_t *handle, __u32 node_block) {
  struct btree_node *node;
  __u32             *children = NULL;
  __u32              i;
  bool               found = false;

  if (node_block == 0 || node_block == INVALID_BLOCK)
    return false;

  if (btree_read_node(sb, handle, node_block, &node) != 0)
    return false;

  if (node->node->num_entries > 0) {
    btree_free_node(node);
    return true;
  }

  // empty node, check children anyway
  if (!node->node->is_leaf) {
    children = bchildren(node, handle);
    for (i = 0; i <= node->node->num_entries && !found; i++)
      found = bsfs_subtree_has_entry(sb, handle, children[i]);
  }

  btree_free_node(node);
  return found;
}

static bool bsfs_empty_dir(struct inode *dir) {
  btree_handle_t *handle = btree_create_dir_entry_handle();
  bool            empty;

  if (!handle)
    return false; // can't check, say non-empty

  handle->root_block = bsfs_i(dir)->btree_root;
  empty              = !bsfs_subtree_has_entry(dir->i_sb, handle, handle->root_block);

  kfree(handle);
  return empty;
}

// empty dir sits at i_nlink 2. stashes parent + name for evict's snapshot like bsfs_unlink
static int bsfs_rmdir(struct inode *dir, struct dentry *dentry) {
  int           err;
  struct inode *inode = d_inode(dentry);

  if (!bsfs_empty_dir(inode))
    return -ENOTEMPTY;

  bsfs_reclaim_if_low(dir);

  bsfs_i(inode)->i_deleted_parent_ino = dir->i_ino;
  strscpy(bsfs_i(inode)->i_deleted_name, dentry->d_name.name, MAX_FILENAME);

  err = bsfs_remove_link(dentry);
  if (err)
    return err;

  inode_dec_link_count(inode); // "."
  inode_dec_link_count(inode); // parent entry, evicts
  inode_dec_link_count(dir);   // our ".."

  return 0;
}

// ".." isn't stored, so a cross-parent move only touches parent link counts. old_inode's i_nlink stays
static int bsfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir,
                       struct dentry *new_dentry, unsigned int flags) {
  struct inode *old_inode = d_inode(old_dentry);
  struct inode *new_inode = d_inode(new_dentry);
  bool          is_dir    = S_ISDIR(old_inode->i_mode);
  int           err;

  if (flags & ~RENAME_NOREPLACE)
    return -EINVAL;

  bsfs_reclaim_if_low(old_dir);

  if (new_inode) {
    if (is_dir && !bsfs_empty_dir(new_inode))
      return -ENOTEMPTY;

    // target is about to be evicted, stash for its snapshot
    bsfs_i(new_inode)->i_deleted_parent_ino = new_dir->i_ino;
    strscpy(bsfs_i(new_inode)->i_deleted_name, new_dentry->d_name.name, MAX_FILENAME);

    err = bsfs_remove_link(new_dentry);
    if (err)
      return err;

    err = bsfs_add_link(new_dentry, old_inode);
    if (err) {
      bsfs_add_link(new_dentry, new_inode); // best effort restore
      return err;
    }

    if (is_dir)
      inode_dec_link_count(new_inode); // "."
    inode_dec_link_count(new_inode);   // dir entry, evicts
  } else {
    err = bsfs_add_link(new_dentry, old_inode);
    if (err)
      return err;
    if (is_dir)
      inode_inc_link_count(new_dir); // our ".."
  }

  err = bsfs_remove_link(old_dentry);
  mark_inode_dirty(old_inode);

  if (is_dir)
    inode_dec_link_count(old_dir); // our ".."

  return err;
}

const struct inode_operations bsfs_dir_inode_operations = {
    .lookup = bsfs_lookup,
    .create = bsfs_create,
    .unlink = bsfs_unlink,
    .mkdir  = bsfs_mkdir,
    // .link		= bsfs_link,
    // .symlink	= bsfs_symlink,
    .rmdir  = bsfs_rmdir,
    .mknod  = bsfs_mknod,
    .rename = bsfs_rename,
    // .getattr	= bsfs_getattr,
    // .tmpfile	= bsfs_tmpfile,
};

static int bsfs_emit_subtree(struct super_block *sb, btree_handle_t *handle, __u32 node_block, struct dir_context *ctx, loff_t *index) {
  __u32 i;

  struct btree_node *node;

  __u32 *children = NULL;

  if (node_block == 0 || node_block == INVALID_BLOCK)
    return 0;

  if (btree_read_node(sb, handle, node_block, &node) != 0)
    return 0;

  // internal entries are separator copies, emitting them lists files twice
  if (!node->node->is_leaf) {
    children = bchildren(node, handle);

    for (i = 0; i <= node->node->num_entries; i++) {
      if (bsfs_emit_subtree(sb, handle, children[i], ctx, index)) {
        btree_free_node(node);
        return 1;
      }
    }

    btree_free_node(node);
    return 0;
  }

  for (i = 0; i < node->node->num_entries; i++) {
    struct bsfs_dentry *de;
    loff_t              entry_pos;
    int                 namelen;

    de = (struct bsfs_dentry *)(bentries(node) + (size_t)i * handle->entry_size);

    // dir_emit_dots handles these
    if (de->filename[0] == '.' && (de->filename[1] == '\0' || (de->filename[1] == '.' && de->filename[2] == '\0')))
      continue;

    // 0,1 reserved for . and ..
    entry_pos = *index + 2;
    (*index)++;

    if (entry_pos < ctx->pos) // already emitted last call
      continue;

    ctx->pos = entry_pos;
    namelen  = strnlen(de->filename, MAX_FILENAME);

    if (!dir_emit(ctx, de->filename, namelen, de->ino, DT_UNKNOWN)) {
      btree_free_node(node);
      // buffer full: resume at this entry next call
      return 1;
    }

    ctx->pos = entry_pos + 1;
  }

  btree_free_node(node);
  return 0;
}

static int bsfs_readdir(struct file *file, struct dir_context *ctx) {
  loff_t index = 0;

  struct inode       *dir = file_inode(file);
  struct super_block *sb  = dir->i_sb;
  btree_handle_t     *handle;

  if (!dir_emit_dots(file, ctx))
    return 0;

  handle = btree_create_dir_entry_handle();

  if (!handle)
    return -ENOMEM;

  handle->root_block = bsfs_i(dir)->btree_root; // 0 is an empty dir
  bsfs_emit_subtree(sb, handle, handle->root_block, ctx, &index);

  kfree(handle);
  return 0;
}

const struct file_operations bsfs_dir_operations = {
    .llseek         = generic_file_llseek,
    .read           = generic_read_dir,
    .iterate_shared = bsfs_readdir,
    .fsync          = bsfs_fsync,
};
