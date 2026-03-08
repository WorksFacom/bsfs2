/**
 * @file directory_operations_bsfs.c
 * @brief Implementation of high-level directory operations for BSFS.
 *
 * This module handles directory creation, deletion, and movement of files and
 * subdirectories between parent directories. It relies on directory entry
 * manipulation and inode handling provided by supporting modules.
 */

#include "../include/dir.h"

/**
 * @brief Creates a new directory at the given path.
 *
 * This function allocates a new inode of type directory and inserts it as an
 * entry into the parent directory determined by the path.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the loaded superblock.
 * @param path Absolute or relative path for the new directory.
 * @param cwd Current working directory (for resolving relative paths).
 * @return 0 on success, -1 on error.
 */
int create_directory(FILE *disk, bsfs_super_block *sb, const char *path,
                     dir_t *cwd) {
  dir_t parent;
  char name[MAX_FILENAME];

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
  inode_tree->root_block = sb->inode_root;
  uint32_t new_inode_number = get_next_inode_number(disk, sb, inode_tree);
  if (new_inode_number == INVALID_BLOCK) {
    fprintf(stderr, "Failed to allocate new inode number.\n");
    free(inode_tree);
    return -1;
  }

  bsfs_inode new_inode;
  memset(&new_inode, 0, sizeof(bsfs_inode));
  new_inode.inode_number = new_inode_number;
  new_inode.file_type = FILE_TYPE_DIR;
  new_inode.permissions = 0755;
  new_inode.links_count = 1;
  new_inode.created_at = new_inode.modified_at = new_inode.accessed_at =
      (uint32_t)time(NULL);

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
  dot.file_type = FILE_TYPE_DIR;
  dot.file_size = 0;
  strncpy(dot.filename, ".", MAX_FILENAME);

  dir_t new_dir;
  memset(&new_dir, 0, sizeof(new_dir));
  new_dir.inode = new_inode;
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
  dotdot.file_type = FILE_TYPE_DIR;
  dotdot.file_size = 0;
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
  entry.file_type = FILE_TYPE_DIR;
  entry.file_size = 0;
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

/**
 * @brief Deletes a directory at the given path, if it is empty.
 *
 * This function removes the directory entry and deallocates the associated
 * inode.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the loaded superblock.
 * @param path Absolute or relative path of the directory to remove.
 * @param cwd Current working directory (for resolving relative paths).
 * @return 0 on success, -1 if the directory is not empty or cannot be removed.
 */
int remove_directory(FILE *disk, bsfs_super_block *sb, const char *path,
                     dir_t *cwd) {
  dir_t parent;
  char name[MAX_FILENAME];

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
  inode_tree->root_block = sb->inode_root;
  bsfs_inode dir_inode;
  if (!read_inode(disk, inode_tree, entry.inode_number, &dir_inode)) {
    fprintf(stderr, "Failed to read directory inode.\n");
    free(inode_tree);
    return -1;
  }

  // 4. Open the directory and check if it's empty
  dir_t dir;
  memset(&dir, 0, sizeof(dir));
  dir.inode = dir_inode;
  dir.current_index = 0;

  btree_handle_t *dir_tree = btree_create_dir_entry_handle();
  int count = 0;
  dir_entry_t temp;

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

/**
 * @brief Moves or renames a directory entry from one directory to another.
 *
 * This function removes an existing directory entry from the source directory
 * and inserts it into the destination directory. The entry can also be renamed
 * in the process.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock.
 * @param src_dir Pointer to the source directory.
 * @param src_name Name of the entry to move (existing name).
 * @param dest_dir Pointer to the destination directory.
 * @param dest_name New name for the entry in the destination (can be same as
 * src_name).
 * @return 0 on success, -1 on failure.
 */
int move_dir_entry(FILE *disk, bsfs_super_block *sb, dir_t *src_dir,
                   const char *src_name, dir_t *dest_dir,
                   const char *dest_name) {
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

/**
 * @brief Initialize and persist the root directory ("/") on a fresh image.
 *
 * Creates the root inode with default ownership and permissions, allocates
 * a directory B-tree for entries, inserts the mandatory "." and ".." entries,
 * and ensures the inode tree contains the root inode.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (root inode and tree roots may be updated).
 * @return 0 on success; -1 on error.
 */
int create_root_directory(FILE *disk, bsfs_super_block *sb) {
  uint32_t root_inode_number = sb->root_inode;

  bsfs_inode root_inode;
  memset(&root_inode, 0, sizeof(bsfs_inode));
  root_inode.inode_number = root_inode_number;
  root_inode.file_type = FILE_TYPE_DIR; // DIRECTORY
  root_inode.links_count = 1;
  root_inode.permissions = 0755;
  root_inode.uid = 0;
  root_inode.gid = 0;
  root_inode.created_at = root_inode.modified_at = root_inode.accessed_at =
      time(NULL);
  root_inode.block_count = 0;
  root_inode.file_size = 0;

  // Initialize directory tree
  btree_handle_t *dir_tree = btree_create_dir_entry_handle();
  root_inode.btree_root = allocate_single_block(disk, sb);

  if (root_inode.btree_root == INVALID_BLOCK) {
    fprintf(stderr, "Failed to allocate block for root directory B-tree.\n");
    free(dir_tree);
    return -1;
  }

  // . and .. entries
  dir_entry_t dot = {
      .inode_number = root_inode_number, .file_type = 1, .file_size = 0};
  strcpy(dot.filename, ".");

  dir_entry_t dotdot = {
      .inode_number = root_inode_number, .file_type = 1, .file_size = 0};
  strcpy(dotdot.filename, "..");

  // Inserção na árvore
  if (btree_insert(disk, sb, dir_tree, &dot) != 0 ||
      btree_insert(disk, sb, dir_tree, &dotdot) != 0) {
    fprintf(stderr, "Failed to insert root directory entries.\n");
    free(dir_tree);
    return -1;
  }

  // Tree on superblock
  sb->inode_root =
      sb->inode_root == 0 ? allocate_single_block(disk, sb) : sb->inode_root;

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
