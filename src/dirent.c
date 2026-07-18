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

// TODO: review - is this working?
int __scan_dir_tree_find(struct super_block *sb, btree_handle_t *handle, __u32 node_block, const char *name, struct bsfs_dentry **output,
                         struct btree_node **out_node) {
  int ret, i;

  struct btree_node  *node;
  struct bsfs_dentry *dirent;

  if (node_block == 0)
    return 0;

  ret = btree_read_node(sb, handle, node_block, &node);

  if (ret != 0)
    return 0;

  // Check current node entries first
  for (i = 0; i < node->node->num_entries; i++) {
    dirent = (struct bsfs_dentry *)(bentries(node) + i * handle->entry_size);

    if (strncmp(dirent->filename, name, MAX_FILENAME) == 0) {
      *output   = dirent;
      *out_node = node;
      return 1;
    }
  }

  // Recurse into children if not a leaf
  int found = 0;
  if (!node->node->is_leaf) {
    __u32 *children = bchildren(node, handle);
    for (i = 0; i <= node->node->num_entries && !found; i++) {
      found = __scan_dir_tree_find(sb, handle, children[i], name, output, out_node);
    }
  }

  btree_free_node(node);
  return found;
}

struct bsfs_dentry *lookup_dir_entry(struct dentry *dentry, struct btree_node **out_node) {
  int ret;

  const char         *name   = dentry->d_name.name;
  struct inode       *dir    = d_inode(dentry->d_parent);
  struct super_block *sb     = dir->i_sb;
  struct bsfs_dentry *dirent = NULL;

  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block     = bsfs_i(dir)->btree_root;

  // TODO: ret useless?
  ret = __scan_dir_tree_find(sb, handle, handle->root_block, name, &dirent, out_node);

  kfree(handle);

  return dirent;
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
  struct btree_node  *node   = NULL;
  struct bsfs_dentry *dirent = NULL;
  int                 err    = 0;

  dirent = lookup_dir_entry(dentry, &node);

  if (!dirent) {
    err = -ENOENT;
    goto out;
  }

  handle = btree_create_dir_entry_handle();

  if (!handle) {
    err = -ENOMEM;
    goto out;
  }

  handle->root_block = bsfs_i(dir)->btree_root;

  if (!btree_remove_entry(sb, handle, dirent)) {
    err = -EIO;
    goto out;
  }

  if (handle->root_block != bsfs_i(dir)->btree_root)
    bsfs_i(dir)->btree_root = handle->root_block;

  inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
  mark_inode_dirty(dir);

out:
  if (node)
    btree_free_node(node);
  kfree(handle);
  return err;
}
