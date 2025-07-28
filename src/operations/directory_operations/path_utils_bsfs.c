/**
 * @file path_utils_bsfs.c
 * @brief Implementation of path resolution utilities for BSFS.
 *
 * This file provides the core logic for resolving filesystem paths, including
 * functions for navigating directories, extracting final components, and
 * retrieving inodes based on path strings.
 */

#include "../../include/path_utils_bsfs.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Resolves a path to its parent directory and the final component name.
 *
 * This function traverses the file system path starting from either the root
 * (for absolute paths) or the given current working directory (cwd), and
 * returns:
 * - The parent directory (dir_t) that contains the final component.
 * - The name of the final component (file or directory name).
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param path Input path string (absolute or relative).
 * @param cwd Pointer to current working directory (used for relative paths).
 * @param parent_out Output parameter to receive the parent directory.
 * @param final_name Output parameter to receive the final name.
 * @return 0 on success, -1 on error (invalid path or not found).
 */
int resolve_path(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd,
                 dir_t *parent_out, char *final_name) {
  if (!path || strlen(path) == 0)
    return -1;

  char path_copy[1024];
  strncpy(path_copy, path, sizeof(path_copy));
  path_copy[sizeof(path_copy) - 1] = '\0';

  char *token;
  char *rest = path_copy;

  dir_t current;
  btree_handle_t *inode_handle = btree_create_inode_handle();

  inode_t temp_inode;

  // Start from root if absolute, or from cwd if relative
  if (path[0] == '/') {
    if (read_inode(disk, inode_handle, sb->root_inode, &temp_inode) != 0) {
      free(inode_handle);
      return -1;
    }
    current.inode = temp_inode;
    current.current_index = 0;
  } else {
    current = *cwd;
  }

  token = strtok(rest, "/");
  while (token != NULL) {
    char *next_token = strtok(NULL, "/");

    if (next_token == NULL) {
      strncpy(final_name, token, MAX_FILENAME);
      *parent_out = current;
      free(inode_handle);
      return 0;
    }

    dir_entry_t entry;
    if (lookup_dir_entry(disk, sb, &current, token, &entry) != 0) {
      free(inode_handle);
      return -1;
    }

    if (read_inode(disk, inode_handle, entry.inode_number, &temp_inode) != 0) {
      free(inode_handle);
      return -1;
    }

    dir_t next_dir;
    next_dir.inode = temp_inode;
    next_dir.current_index = 0;

    if (next_dir.inode.file_type != 0) {
      free(inode_handle);
      return -1;
    }

    current = next_dir;
    token = next_token;
  }

  free(inode_handle);
  return -1;
}

/**
 * @brief Looks up a file or directory by path and loads its inode.
 *
 * This function resolves the given path and attempts to locate the
 * corresponding inode (for either a file or directory). It internally uses
 * `resolve_path` and `lookup_dir_entry`.
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param path Input path string (absolute or relative).
 * @param cwd Pointer to current working directory (used for relative paths).
 * @param out_inode Output parameter to receive the found inode.
 * @return 0 on success, -1 if the file or directory is not found.
 */
int lookup_file(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd,
                inode_t *out_inode) {
  dir_t parent;
  char name[MAX_FILENAME];

  if (resolve_path(disk, sb, path, cwd, &parent, name) != 0) {
    return -1;
  }

  dir_entry_t entry;
  if (lookup_dir_entry(disk, sb, &parent, name, &entry) != 0) {
    return -1;
  }

  btree_handle_t *inode_handle = btree_create_inode_handle();
  int result = read_inode(disk, inode_handle, entry.inode_number, out_inode);
  free(inode_handle);
  return result;
}
