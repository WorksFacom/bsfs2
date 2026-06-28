/**
 * @file directory_operations_bsfs.c
 * @brief Implementation of high-level directory operations for BSFS.
 *
 * This module handles directory creation, deletion, and movement of files and
 * subdirectories between parent directories. It relies on directory entry
 * manipulation and inode handling provided by supporting modules.
 */

#include "../include/dir.h"
#include "../bsfs.h"
#include "../include/btree.h"
#include "../include/dirent.h"
#include "../include/inode.h"
#include "../include/rec_entry.h"

// TODO: .rmdir
// int remove_directory(FILE *disk, bsfs_super_block *sb, const char *path, dir_t *cwd) {
//   dir_t parent;
//   char  name[MAX_FILENAME];
//
//   // 1. Resolve parent and name
//   if (resolve_path(disk, sb, path, cwd, &parent, name) != 0) {
//     fprintf(stderr, "Path resolution failed: %s\n", path);
//     return -1;
//   }
//
//   // 2. Lookup entry
//   dir_entry_t entry;
//   if (lookup_dir_entry(disk, sb, &parent, name, &entry) != 0) {
//     fprintf(stderr, "Directory entry not found: %s\n", name);
//     return -1;
//   }
//
//   if (entry.file_type != FILE_TYPE_DIR) {
//     fprintf(stderr, "Target is not a directory.\n");
//     return -1;
//   }
//
//   // 3. Load inode
//   btree_handle_t *inode_tree = btree_create_inode_handle();
//   inode_tree->root_block     = sb->inode_root;
//   bsfs_inode dir_inode;
//   if (!read_inode(disk, inode_tree, entry.inode_number, &dir_inode)) {
//     fprintf(stderr, "Failed to read directory inode.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   // 4. Open the directory and check if it's empty
//   dir_t dir;
//   memset(&dir, 0, sizeof(dir));
//   dir.inode         = dir_inode;
//   dir.current_index = 0;
//
//   btree_handle_t *dir_tree = btree_create_dir_entry_handle();
//   int             count    = 0;
//   dir_entry_t     temp;
//
//   while (read_dir_entry(disk, sb, &dir, dir_tree, &temp) == 0) {
//     if (strcmp(temp.filename, ".") != 0 && strcmp(temp.filename, "..") != 0) {
//       count++;
//       break;
//     }
//   }
//
//   free(dir_tree);
//
//   if (count > 0) {
//     fprintf(stderr, "Directory is not empty.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   // 5. Remove directory entry from parent
//   if (remove_dir_entry(disk, sb, &parent, name) != 0) {
//     fprintf(stderr, "Failed to remove directory entry from parent.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   // 6. Remove inode (remove_inode returns 1 on success)
//   if (!remove_inode(disk, sb, inode_tree, entry.inode_number)) {
//     fprintf(stderr, "Failed to remove inode.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   free(inode_tree);
//   return 0;
// }

// TODO: .rename
// int move_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *src_dir, const char *src_name, dir_t *dest_dir, const char *dest_name) {
//   // 1. Lookup entry in source
//   dir_entry_t entry;
//   if (lookup_dir_entry(disk, sb, src_dir, src_name, &entry) != 0) {
//     fprintf(stderr, "Entry '%s' not found in source directory.\n", src_name);
//     return -1;
//   }
//
//   // 2. Remove from source directory
//   if (remove_dir_entry(disk, sb, src_dir, src_name) != 0) {
//     fprintf(stderr, "Failed to remove entry '%s' from source.\n", src_name);
//     return -1;
//   }
//
//   // 3. Update name if needed
//   if (strcmp(src_name, dest_name) != 0) {
//     strncpy(entry.filename, dest_name, MAX_FILENAME);
//     entry.filename[MAX_FILENAME - 1] = '\0'; // Ensure NUL termination
//   }
//
//   // 4. Add to destination directory
//   if (add_dir_entry(disk, sb, dest_dir, &entry) != 0) {
//     fprintf(stderr, "Failed to insert entry into destination directory.\n");
//     return -1;
//   }
//
//   return 0;
// }

ino_t bsfs_inode_by_name(struct dentry *dentry) {
  ino_t ino = 0;

  struct btree_node *node;

  struct bsfs_dentry *dirent = lookup_dir_entry(dentry, &node);

  if (dirent) {
    ino = dirent->ino;
    btree_free_node(node);
  }

  return ino;
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

static int bsfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t rdev) {
  struct inode *inode;

  // TODO: old -> new?
  if (!old_valid_dev(rdev))
    return -EINVAL;

  inode = bsfs_new_inode(dir, mode);

  if (IS_ERR(inode))
    return PTR_ERR(inode);

  bsfs_set_inode(inode, rdev);

  mark_inode_dirty(inode);

  return link_nondir(dentry, inode);
}

static int bsfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
  return bsfs_mknod(&nop_mnt_idmap, dir, dentry, mode, 0);
}

static struct dentry *bsfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
  int err;

  struct inode *inode;

  inode = bsfs_new_inode(dir, S_IFDIR | mode);

  if (IS_ERR(inode))
    return ERR_CAST(inode);

  inode_inc_link_count(dir); // .. in the new dir references the parent
  bsfs_set_inode(inode, 0);
  inode_inc_link_count(inode); // . self-link: i_nlink 1 -> 2

  mark_inode_dirty(inode);

  // . and .. are synthesized by bsfs_readdir/the dcache, so no on-disk dot
  // entries are created here - only the new name is added to the parent
  err = bsfs_add_link(dentry, inode);

  if (err)
    goto out_fail;

  d_instantiate(dentry, inode);
out:
  return ERR_PTR(err);

out_fail:
  inode_dec_link_count(inode);
  inode_dec_link_count(inode);
  iput(inode);
  inode_dec_link_count(dir);
  goto out;
}

// Recovery snapshot is not built here: a name can be removed while the file is still open (and being written to) or while other hard links
// remain. We only want a snapshot once the inode is truly gone, so we stash the parent ino + name and let bsfs_evict_inode build it from
// the inode's final state when i_nlink == 0
static int bsfs_unlink(struct inode *dir, struct dentry *dentry) {
  int err;

  struct inode *inode = d_inode(dentry);

  if (S_ISDIR(inode->i_mode))
    return -EISDIR;

  // hand the parent ino + name to bsfs_evict_inode for the recovery snapshot.
  bsfs_i(inode)->i_deleted_parent_ino = dir->i_ino;
  strscpy(bsfs_i(inode)->i_deleted_name, dentry->d_name.name, MAX_FILENAME);

  err = bsfs_remove_link(dentry);
  if (err)
    return err;

  inode_dec_link_count(inode);

  return 0;
}

const struct inode_operations bsfs_dir_inode_operations = {
    .lookup = bsfs_lookup,
    .create = bsfs_create,
    .unlink = bsfs_unlink,
    .mkdir  = bsfs_mkdir,
    // .link		= bsfs_link,
    // .symlink	= bsfs_symlink,
    // .rmdir		= bsfs_rmdir,
    // .mknod		= bsfs_mknod,
    // .rename		= bsfs_rename,
    // .getattr	= bsfs_getattr,
    // .tmpfile	= bsfs_tmpfile,
};

static int bsfs_emit_subtree(struct super_block *sb, btree_handle_t *handle, __u32 node_block, struct dir_context *ctx, loff_t *index) {
  __u32 i;

  struct btree_node *node;

  __u32 *children = NULL;

  if (node_block == 0 || node_block == INVALID_BLOCK)
    return 0;

  if (btree_read_node(sb, node_block, &node) != 0)
    return 0;

  if (!node->node->is_leaf)
    children = bchildren(node, handle);

  for (i = 0; i < node->node->num_entries; i++) {
    struct bsfs_dentry *de;
    loff_t              entry_pos;
    int                 namelen;

    if (!node->node->is_leaf && bsfs_emit_subtree(sb, handle, children[i], ctx, index)) {
      btree_free_node(node);
      return 1;
    }

    de = (struct bsfs_dentry *)(bentries(node) + (size_t)i * handle->entry_size);

    // . and .. are emitted via dir_emit_dots
    if (de->filename[0] == '.' && (de->filename[1] == '\0' || (de->filename[1] == '.' && de->filename[2] == '\0')))
      continue;

    // 0,1 reserved for . and ..
    entry_pos = *index + 2;
    (*index)++;

    if (entry_pos < ctx->pos) // already delivered on a previous call
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

  if (!node->node->is_leaf && bsfs_emit_subtree(sb, handle, children[node->node->num_entries], ctx, index)) {
    btree_free_node(node);
    return 1;
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

  handle->root_block = bsfs_i(dir)->btree_root; // 0 => empty dir, no-op
                                                //
  bsfs_emit_subtree(sb, handle, handle->root_block, ctx, &index);

  kfree(handle);
  return 0;
}

const struct file_operations bsfs_dir_operations = {
    .llseek         = generic_file_llseek,
    .read           = generic_read_dir,
    .iterate_shared = bsfs_readdir,
    .fsync          = noop_fsync, // TODO: implement
};
