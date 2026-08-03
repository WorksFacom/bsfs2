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

// static int btree_find_min_entry(FILE *disk, btree_handle_t *handle, uint32_t node_block, void *out_entry) {
//   if (node_block == 0)
//     return 0;
//   btree_node_t *node = btree_read_node(disk, handle, node_block);
//   if (!node)
//     return 0;
//   int ok;
//   if (node->is_leaf) {
//     if (node->num_entries > 0) {
//       memcpy(out_entry, node->entries, handle->entry_size);
//       ok = 1;
//     } else {
//       ok = 0;
//     }
//   } else {
//     ok = btree_find_min_entry(disk, handle, node->children[0], out_entry);
//   }
//   btree_free_node(node);
//   return ok;
// }

// static int btree_find_successor_entry(FILE *disk, btree_handle_t *handle, uint32_t node_block, uint32_t after_key, void *out_entry) {
//   if (node_block == 0)
//     return 0;
//   btree_node_t *node = btree_read_node(disk, handle, node_block);
//   if (!node)
//     return 0;
//   int      found   = 0;
//   uint8_t *entries = node->entries;
//
//   for (uint32_t i = 0; i < node->num_entries && !found; i++) {
//     uint32_t ekey = handle->get_key(entries + i * handle->entry_size);
//     if (ekey > after_key) {
//       if (!node->is_leaf) {
//         // Try to find in left child first
//         if (btree_find_successor_entry(disk, handle, node->children[i], after_key, out_entry)) {
//           found = 1;
//           break;
//         }
//       }
//       // Current entry is the successor
//       memcpy(out_entry, entries + i * handle->entry_size, handle->entry_size);
//       found = 1;
//       break;
//     }
//   }
//
//   // If not found, search in the rightmost child
//   if (!found && !node->is_leaf) {
//     found = btree_find_successor_entry(disk, handle, node->children[node->num_entries], after_key, out_entry);
//   }
//
//   btree_free_node(node);
//   return found;
// }

// int read_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *dir, btree_handle_t *handle, dir_entry_t *out) {
//   (void)sb;
//   (void)handle;
//   if (!disk || !dir || !out)
//     return -1;
//
//   if (dir->inode.btree_root == 0)
//     return -1;
//
//   btree_handle_t *dir_handle = btree_create_dir_entry_handle();
//   dir_handle->root_block     = dir->inode.btree_root;
//
//   int ok = 0;
//   if (!dir->iter_started) {
//     ok = btree_find_min_entry(disk, dir_handle, dir_handle->root_block, out);
//     if (ok) {
//       dir->iter_started = 1;
//       dir->iter_key     = hash_filename(out->filename);
//     }
//   } else {
//     ok = btree_find_successor_entry(disk, dir_handle, dir_handle->root_block, dir->iter_key, out);
//     if (ok) {
//       dir->iter_key = hash_filename(out->filename);
//     }
//   }
//
//   free(dir_handle);
//   if (!ok) {
//     dir->iter_started = 0;
//     dir->iter_key     = 0;
//     return -1;
//   }
//   return 0;
// }

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
