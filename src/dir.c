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
#include "../include/inode.h"

// TODO: .mkdir
// int create_directory(FILE *disk, bsfs_super_block *sb, const char *path, dir_t *cwd) {
//   dir_t parent;
//   char  name[MAX_FILENAME];
//
//   // 1. Resolve the parent directory and final component
//   if (resolve_path(disk, sb, path, cwd, &parent, name) != 0) {
//     fprintf(stderr, "Failed to resolve path: %s\n", path);
//     return -1;
//   }
//
//   // 2. Check if an entry with the same name already exists
//   dir_entry_t existing;
//   if (lookup_dir_entry(disk, sb, &parent, name, &existing) == 0) {
//     fprintf(stderr, "Entry already exists: %s\n", name);
//     return -1;
//   }
//
//   // 3. Prepare inode handle and allocate inode number
//   btree_handle_t *inode_tree = btree_create_inode_handle();
//   inode_tree->root_block     = sb->inode_root;
//   uint32_t new_inode_number  = get_next_inode_number(disk, sb, inode_tree);
//   if (new_inode_number == INVALID_BLOCK) {
//     fprintf(stderr, "Failed to allocate new inode number.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   bsfs_inode new_inode;
//   memset(&new_inode, 0, sizeof(bsfs_inode));
//   new_inode.inode_number = new_inode_number;
//   new_inode.file_type    = FILE_TYPE_DIR;
//   new_inode.permissions  = 0755;
//   new_inode.links_count  = 1;
//   new_inode.created_at = new_inode.modified_at = new_inode.accessed_at = (uint32_t)time(NULL);
//
//   // 4. Insert the inode into the inode tree
//   if (insert_inode(disk, sb, inode_tree, &new_inode) != 0) {
//     fprintf(stderr, "Failed to insert new inode.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   // 5. Create "." entry pointing to self
//   dir_entry_t dot;
//   memset(&dot, 0, sizeof(dir_entry_t));
//   dot.inode_number = new_inode_number;
//   dot.file_type    = FILE_TYPE_DIR;
//   dot.file_size    = 0;
//   strncpy(dot.filename, ".", MAX_FILENAME);
//
//   dir_t new_dir;
//   memset(&new_dir, 0, sizeof(new_dir));
//   new_dir.inode         = new_inode;
//   new_dir.current_index = 0;
//
//   if (add_dir_entry(disk, sb, &new_dir, &dot) != 0) {
//     fprintf(stderr, "Failed to add '.' entry.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   // 6. Create ".." entry pointing to parent
//   dir_entry_t dotdot;
//   memset(&dotdot, 0, sizeof(dir_entry_t));
//   dotdot.inode_number = parent.inode.inode_number;
//   dotdot.file_type    = FILE_TYPE_DIR;
//   dotdot.file_size    = 0;
//   strncpy(dotdot.filename, "..", MAX_FILENAME);
//
//   if (add_dir_entry(disk, sb, &new_dir, &dotdot) != 0) {
//     fprintf(stderr, "Failed to add '..' entry.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   // 7. Add this new directory as an entry to the parent
//   dir_entry_t entry;
//   memset(&entry, 0, sizeof(dir_entry_t));
//   entry.inode_number = new_inode_number;
//   entry.file_type    = FILE_TYPE_DIR;
//   entry.file_size    = 0;
//   strncpy(entry.filename, name, MAX_FILENAME);
//   entry.filename[MAX_FILENAME - 1] = '\0';
//
//   if (add_dir_entry(disk, sb, &parent, &entry) != 0) {
//     fprintf(stderr, "Failed to add directory entry to parent.\n");
//     free(inode_tree);
//     return -1;
//   }
//
//   free(inode_tree);
//   return 0;
// }

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

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
static unsigned minix_last_byte(struct inode *inode, unsigned long page_nr) {
  unsigned last_byte = PAGE_SIZE;

  if (page_nr == (inode->i_size >> PAGE_SHIFT))
    last_byte = inode->i_size & (PAGE_SIZE - 1);
  return last_byte;
}

static void *dir_get_folio(struct inode *dir, unsigned long n, struct folio **foliop) {
  struct folio *folio = read_mapping_folio(dir->i_mapping, n, NULL);

  if (IS_ERR(folio))
    return ERR_CAST(folio);
  *foliop = folio;
  return kmap_local_folio(folio, 0);
}

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

static int bsfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t rdev) {
  struct inode *inode;

  // TODO: old -> new?
  if (!old_valid_dev(rdev))
    return -EINVAL;

  inode = bsfs_new_inode(dir, mode);

  if (IS_ERR(inode))
    return PTR_ERR(inode);

  minix_set_inode(inode, rdev);

  mark_inode_dirty(inode);

  return add_nondir(dentry, inode);
}

static int bsfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
  return bsfs_mknod(&nop_mnt_idmap, dir, dentry, mode, 0);
}

const struct inode_operations bsfs_dir_inode_operations = {
    .lookup = bsfs_lookup,
    .create = bsfs_create
    // .link		= bsfs_link,
    // .unlink		= bsfs_unlink,
    // .symlink	= bsfs_symlink,
    // .mkdir		= bsfs_mkdir,
    // .rmdir		= bsfs_rmdir,
    // .mknod		= bsfs_mknod,
    // .rename		= bsfs_rename,
    // .getattr	= bsfs_getattr,
    // .tmpfile	= bsfs_tmpfile,
};
