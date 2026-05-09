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

int create_directory(FILE *disk, bsfs_super_block *sb, const char *path, dir_t *cwd) {
  dir_t parent;
  char  name[MAX_FILENAME];

  // 1. Resolve the parent directory and final component
  if (resolve_path(disk, sb, path, cwd, &parent, name) != 0) {
    fprintf(stderr, "Failed to resolve path: %s\n", path);
    return -1;
  }

  // 2. Check if an entry with the same name already exists
  dir_entry_t existing;
  if (lookup_dir_entry(disk, sb, &parent, name, &existing) == 0) {
    fprintf(stderr, "Entry already exists: %s\n", name);
    return -1;
  }

  // 3. Prepare inode handle and allocate inode number
  btree_handle_t *inode_tree = btree_create_inode_handle();
  inode_tree->root_block     = sb->inode_root;
  uint32_t new_inode_number  = get_next_inode_number(disk, sb, inode_tree);
  if (new_inode_number == INVALID_BLOCK) {
    fprintf(stderr, "Failed to allocate new inode number.\n");
    free(inode_tree);
    return -1;
  }

  bsfs_inode new_inode;
  memset(&new_inode, 0, sizeof(bsfs_inode));
  new_inode.inode_number = new_inode_number;
  new_inode.file_type    = FILE_TYPE_DIR;
  new_inode.permissions  = 0755;
  new_inode.links_count  = 1;
  new_inode.created_at = new_inode.modified_at = new_inode.accessed_at = (uint32_t)time(NULL);

  // 4. Insert the inode into the inode tree
  if (insert_inode(disk, sb, inode_tree, &new_inode) != 0) {
    fprintf(stderr, "Failed to insert new inode.\n");
    free(inode_tree);
    return -1;
  }

  // 5. Create "." entry pointing to self
  dir_entry_t dot;
  memset(&dot, 0, sizeof(dir_entry_t));
  dot.inode_number = new_inode_number;
  dot.file_type    = FILE_TYPE_DIR;
  dot.file_size    = 0;
  strncpy(dot.filename, ".", MAX_FILENAME);

  dir_t new_dir;
  memset(&new_dir, 0, sizeof(new_dir));
  new_dir.inode         = new_inode;
  new_dir.current_index = 0;

  if (add_dir_entry(disk, sb, &new_dir, &dot) != 0) {
    fprintf(stderr, "Failed to add '.' entry.\n");
    free(inode_tree);
    return -1;
  }

  // 6. Create ".." entry pointing to parent
  dir_entry_t dotdot;
  memset(&dotdot, 0, sizeof(dir_entry_t));
  dotdot.inode_number = parent.inode.inode_number;
  dotdot.file_type    = FILE_TYPE_DIR;
  dotdot.file_size    = 0;
  strncpy(dotdot.filename, "..", MAX_FILENAME);

  if (add_dir_entry(disk, sb, &new_dir, &dotdot) != 0) {
    fprintf(stderr, "Failed to add '..' entry.\n");
    free(inode_tree);
    return -1;
  }

  // 7. Add this new directory as an entry to the parent
  dir_entry_t entry;
  memset(&entry, 0, sizeof(dir_entry_t));
  entry.inode_number = new_inode_number;
  entry.file_type    = FILE_TYPE_DIR;
  entry.file_size    = 0;
  strncpy(entry.filename, name, MAX_FILENAME);
  entry.filename[MAX_FILENAME - 1] = '\0';

  if (add_dir_entry(disk, sb, &parent, &entry) != 0) {
    fprintf(stderr, "Failed to add directory entry to parent.\n");
    free(inode_tree);
    return -1;
  }

  free(inode_tree);
  return 0;
}

int remove_directory(FILE *disk, bsfs_super_block *sb, const char *path, dir_t *cwd) {
  dir_t parent;
  char  name[MAX_FILENAME];

  // 1. Resolve parent and name
  if (resolve_path(disk, sb, path, cwd, &parent, name) != 0) {
    fprintf(stderr, "Path resolution failed: %s\n", path);
    return -1;
  }

  // 2. Lookup entry
  dir_entry_t entry;
  if (lookup_dir_entry(disk, sb, &parent, name, &entry) != 0) {
    fprintf(stderr, "Directory entry not found: %s\n", name);
    return -1;
  }

  if (entry.file_type != FILE_TYPE_DIR) {
    fprintf(stderr, "Target is not a directory.\n");
    return -1;
  }

  // 3. Load inode
  btree_handle_t *inode_tree = btree_create_inode_handle();
  inode_tree->root_block     = sb->inode_root;
  bsfs_inode dir_inode;
  if (!read_inode(disk, inode_tree, entry.inode_number, &dir_inode)) {
    fprintf(stderr, "Failed to read directory inode.\n");
    free(inode_tree);
    return -1;
  }

  // 4. Open the directory and check if it's empty
  dir_t dir;
  memset(&dir, 0, sizeof(dir));
  dir.inode         = dir_inode;
  dir.current_index = 0;

  btree_handle_t *dir_tree = btree_create_dir_entry_handle();
  int             count    = 0;
  dir_entry_t     temp;

  while (read_dir_entry(disk, sb, &dir, dir_tree, &temp) == 0) {
    if (strcmp(temp.filename, ".") != 0 && strcmp(temp.filename, "..") != 0) {
      count++;
      break;
    }
  }

  free(dir_tree);

  if (count > 0) {
    fprintf(stderr, "Directory is not empty.\n");
    free(inode_tree);
    return -1;
  }

  // 5. Remove directory entry from parent
  if (remove_dir_entry(disk, sb, &parent, name) != 0) {
    fprintf(stderr, "Failed to remove directory entry from parent.\n");
    free(inode_tree);
    return -1;
  }

  // 6. Remove inode (remove_inode returns 1 on success)
  if (!remove_inode(disk, sb, inode_tree, entry.inode_number)) {
    fprintf(stderr, "Failed to remove inode.\n");
    free(inode_tree);
    return -1;
  }

  free(inode_tree);
  return 0;
}

int move_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *src_dir, const char *src_name, dir_t *dest_dir, const char *dest_name) {
  // 1. Lookup entry in source
  dir_entry_t entry;
  if (lookup_dir_entry(disk, sb, src_dir, src_name, &entry) != 0) {
    fprintf(stderr, "Entry '%s' not found in source directory.\n", src_name);
    return -1;
  }

  // 2. Remove from source directory
  if (remove_dir_entry(disk, sb, src_dir, src_name) != 0) {
    fprintf(stderr, "Failed to remove entry '%s' from source.\n", src_name);
    return -1;
  }

  // 3. Update name if needed
  if (strcmp(src_name, dest_name) != 0) {
    strncpy(entry.filename, dest_name, MAX_FILENAME);
    entry.filename[MAX_FILENAME - 1] = '\0'; // Ensure NUL termination
  }

  // 4. Add to destination directory
  if (add_dir_entry(disk, sb, dest_dir, &entry) != 0) {
    fprintf(stderr, "Failed to insert entry into destination directory.\n");
    return -1;
  }

  return 0;
}

int create_root_directory(FILE *disk, bsfs_super_block *sb) {
  uint32_t root_inode_number = sb->root_inode;

  bsfs_inode root_inode;
  memset(&root_inode, 0, sizeof(bsfs_inode));
  root_inode.inode_number = root_inode_number;
  root_inode.file_type    = FILE_TYPE_DIR; // DIRECTORY
  root_inode.links_count  = 1;
  root_inode.permissions  = 0755;
  root_inode.uid          = 0;
  root_inode.gid          = 0;
  root_inode.created_at = root_inode.modified_at = root_inode.accessed_at = time(NULL);
  root_inode.block_count                                                  = 0;
  root_inode.file_size                                                    = 0;

  // Initialize directory tree
  btree_handle_t *dir_tree = btree_create_dir_entry_handle();
  root_inode.btree_root    = allocate_single_block(disk, sb);

  if (root_inode.btree_root == INVALID_BLOCK) {
    fprintf(stderr, "Failed to allocate block for root directory B-tree.\n");
    free(dir_tree);
    return -1;
  }

  // . and .. entries
  dir_entry_t dot = {.inode_number = root_inode_number, .file_type = 1, .file_size = 0};
  strcpy(dot.filename, ".");

  dir_entry_t dotdot = {.inode_number = root_inode_number, .file_type = 1, .file_size = 0};
  strcpy(dotdot.filename, "..");

  // Inserção na árvore
  if (btree_insert(disk, sb, dir_tree, &dot) != 0 || btree_insert(disk, sb, dir_tree, &dotdot) != 0) {
    fprintf(stderr, "Failed to insert root directory entries.\n");
    free(dir_tree);
    return -1;
  }

  // Tree on superblock
  sb->inode_root = sb->inode_root == 0 ? allocate_single_block(disk, sb) : sb->inode_root;

  btree_handle_t *inode_tree = btree_create_inode_handle();

  if (insert_inode(disk, sb, inode_tree, &root_inode) != 0) {
    fprintf(stderr, "Failed to insert root inode.\n");
    free(dir_tree);
    free(inode_tree);
    return -1;
  }

  free(dir_tree);
  free(inode_tree);
  return 0;
}

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

bsfs_dentry *minix_find_entry(struct dentry *dentry, struct folio **foliop) {
  const char           *name    = dentry->d_name.name;
  int                   namelen = dentry->d_name.len;
  struct inode         *dir     = d_inode(dentry->d_parent);
  struct super_block   *sb      = dir->i_sb;
  struct minix_sb_info *sbi     = minix_sb(sb);
  unsigned long         n;
  unsigned long         npages = dir_pages(dir);
  char                 *p;

  char *namx;
  __u32 inumber;

  for (n = 0; n < npages; n++) {
    char *kaddr, *limit;

    kaddr = dir_get_folio(dir, n, foliop);
    if (IS_ERR(kaddr))
      continue;

    limit = kaddr + minix_last_byte(dir, n) - sbi->s_dirsize;
    for (p = kaddr; p <= limit; p = minix_next_entry(p, sbi)) {
      if (sbi->s_version == MINIX_V3) {
        minix3_dirent *de3 = (minix3_dirent *)p;
        namx               = de3->name;
        inumber            = de3->inode;
      } else {
        minix_dirent *de = (minix_dirent *)p;
        namx             = de->name;
        inumber          = de->inode;
      }
      if (!inumber)
        continue;
      if (namecompare(namelen, sbi->s_namelen, name, namx))
        goto found;
    }
    folio_release_kmap(*foliop, kaddr);
  }
  return NULL;

found:
  return (minix_dirent *)p;
}

ino_t bsfs_inode_by_name(struct dentry *dentry) {
  ino_t         res = 0;
  struct folio *folio;

  struct bsfs_dentry *de = minix_find_entry(dentry, &folio);

  if (de) {
    struct inode *inode = folio->mapping->host;

    res = de->ino;

    folio_release_kmap(folio, de);
  }
  return res;
}

static struct dentry *bsfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
  ino_t         ino;
  struct inode *inode = NULL;

  // TODO: fail if dentry->d_name.len if greater than bsfs supported value

  ino = bsfs_inode_by_name(dentry);

  if (ino)
    inode = bsfs_get_vfs_inode(dir->i_sb, ino);

  return d_splice_alias(inode, dentry);
}

const struct inode_operations minix_dir_inode_operations = {
    .lookup = bsfs_lookup
    // .create		= bsfs_create,
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
