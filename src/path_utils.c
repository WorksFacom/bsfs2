/**
 * @file path_utils_bsfs.c
 * @brief Implementation of path resolution utilities for BSFS.
 *
 * This file provides the core logic for resolving filesystem paths, including
 * functions for navigating directories, extracting final components, and
 * retrieving inodes based on path strings.
 */

#include "../include/path_utils.h"
#include "../include/inode.h"
#include "../include/permissions.h"

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
int resolve_path(FILE *disk, bsfs_super_block *sb, const char *path, dir_t *cwd,
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
  inode_handle->root_block = sb->inode_root;

  bsfs_inode temp_inode;

  // Start from root if absolute, or from cwd if relative
  if (path[0] == '/') {
    if (!read_inode(disk, inode_handle, sb->root_inode, &temp_inode)) {
      free(inode_handle);
      return -1;
    }
    current.inode = temp_inode;
    current.current_index = 0;
  } else {
    // Refresh CWD inode from the inode tree to ensure we see updated
    // directory B-tree root after previous operations (e.g., splits/rebuilds)
    if (!read_inode(disk, inode_handle, cwd->inode.inode_number, &temp_inode)) {
      free(inode_handle);
      return -1;
    }
    current.inode = temp_inode;
    current.current_index = 0;
  }

  token = strtok(rest, "/");
  uint16_t uid = 0, gid = 0; /* default to root here; API-level will enforce */
  while (token != NULL) {
    char *next_token = strtok(NULL, "/");

    if (next_token == NULL) {
      strncpy(final_name, token, MAX_FILENAME);
      *parent_out = current;
      free(inode_handle);
      return 0;
    }

    // require traverse on current directory before descending
    if (!bsfs_can_traverse_dir(&current.inode, uid, gid)) {
      free(inode_handle);
      return -1;
    }

    dir_entry_t entry;
    if (lookup_dir_entry(disk, sb, &current, token, &entry) != 0) {
      free(inode_handle);
      return -1;
    }

    if (!read_inode(disk, inode_handle, entry.inode_number, &temp_inode)) {
      free(inode_handle);
      return -1;
    }

    dir_t next_dir;
    next_dir.inode = temp_inode;
    next_dir.current_index = 0;

    if (next_dir.inode.file_type != FILE_TYPE_DIR) {
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
int lookup_file(FILE *disk, bsfs_super_block *sb, const char *path, dir_t *cwd,
                bsfs_inode *out_inode) {
  if (!disk || !sb || !path || !cwd || !out_inode)
    return -1;

  // Handle special cases: root "/" and current "."
  if (path[0] == '/' && path[1] == '\0') {
    btree_handle_t *inode_handle = btree_create_inode_handle();
    inode_handle->root_block = sb->inode_root;
    int ok = read_inode(disk, inode_handle, sb->root_inode, out_inode);
    free(inode_handle);
    return ok ? 0 : -1;
  }
  if ((path[0] == '.' && path[1] == '\0')) {
    *out_inode = cwd->inode;
    return 0;
  }
  dir_t parent;
  char name[MAX_FILENAME];

  if (resolve_path(disk, sb, path, cwd, &parent, name) != 0) {
    errno = ENOENT;
    return -1;
  }

  dir_entry_t entry;
  if (lookup_dir_entry(disk, sb, &parent, name, &entry) != 0) {
    // Fallback: deep scan to tolerate internal-node ordering differences
    btree_handle_t *dir_h = btree_create_dir_entry_handle();
    dir_h->root_block = parent.inode.btree_root;
    if (!scan_dir_tree_find(disk, dir_h, dir_h->root_block, name, &entry)) {
      errno = ENOENT;
      free(dir_h);
      return -1;
    }
    free(dir_h);
  }

  btree_handle_t *inode_handle = btree_create_inode_handle();
  inode_handle->root_block = sb->inode_root;
  int found = read_inode(disk, inode_handle, entry.inode_number, out_inode);
  free(inode_handle);
  return found ? 0 : -1;
}

/**
 * @brief Normalize a path by collapsing separators and resolving "." and "..".
 *
 * This utility converts a possibly messy path into a normalized textual form:
 *  - Collapses repeated '/' (e.g., "a//b///c" → "a/b/c").
 *  - Removes "." components.
 *  - Resolves ".." by popping the previous component when possible.
 *  - Preserves whether the path is absolute (starts with '/').
 *
 * The function operates purely on the string level:
 *  - If @p path is absolute, the result starts with '/'.
 *  - If @p path is relative, the result is a relative normalized string.
 *  - No filesystem lookups are performed here (i.e., this does not check
 *    whether components exist); path-to-inode resolution must be done later.
 *
 * Memory ownership:
 *  - Returns a newly allocated NUL-terminated string that the caller MUST
 * free().
 *
 * Error handling:
 *  - Returns NULL on invalid input or allocation failure.
 *
 * Examples:
 *  - normalize_path("/a//b/./c/../d") → "/a/b/d"
 *  - normalize_path("foo/./bar//baz/..") → "foo/bar"
 *  - normalize_path("/") → "/"
 *  - normalize_path("..") → ""  (empty relative path after resolving)
 *
 * @param path Input path string (absolute or relative).
 * @return Newly allocated normalized string on success; NULL on error.
 */
static char *normalize_path(const char *path) {
  if (!path || !*path)
    return NULL;

  /* Working copy buffer: tokenize component-by-component.
     Upper bound: original length + 1 for the terminating NUL. */
  size_t n = strlen(path);
  char *tmp = (char *)malloc(n + 1);
  if (!tmp)
    return NULL;
  memcpy(tmp, path, n + 1);

  /* Simple stack of component pointers (pointing inside tmp). */
  char **stack = (char **)malloc(sizeof(char *) * (n + 1));
  if (!stack) {
    free(tmp);
    return NULL;
  }
  size_t top = 0;

  int absolute = (tmp[0] == '/');

  /* Tokenize by '/', applying textual rules for "." and "..". */
  char *save = NULL;
  for (char *tok = strtok_r(tmp, "/", &save); tok;
       tok = strtok_r(NULL, "/", &save)) {
    if (strcmp(tok, ".") == 0) {
      /* Skip current directory. */
      continue;
    } else if (strcmp(tok, "..") == 0) {
      if (top > 0) {
        /* Pop one component if we have any accumulated. */
        top--;
      }
    } else if (*tok != '\0') {
      /* Keep normal components. */
      stack[top++] = tok;
    }
  }

  /* Compute output length. Start with 1 to account for a possible leading '/'.
   */
  size_t outlen = 1;
  for (size_t i = 0; i < top; i++)
    outlen += strlen(stack[i]) + 1;
  if (!absolute && outlen == 1)
    outlen = (top == 0 ? 1 : outlen); /* empty relative path allowed */

  char *norm = (char *)malloc(outlen + 1);
  if (!norm) {
    free(stack);
    free(tmp);
    return NULL;
  }

  char *p = norm;
  if (absolute)
    *p++ = '/';

  for (size_t i = 0; i < top; i++) {
    size_t L = strlen(stack[i]);
    memcpy(p, stack[i], L);
    p += L;
    if (i + 1 < top)
      *p++ = '/';
  }
  *p = '\0';

  /* Special cases:
     - Absolute with no components → "/"
     - Relative with no components → "" (empty) */
  if (absolute && top == 0) {
    norm[0] = '/';
    norm[1] = '\0';
  }
  if (!absolute && top == 0) {
    norm[0] = '\0';
  }

  free(stack);
  free(tmp);
  return norm;
}

/**
 * @brief Split a normalized path into parent path and basename.
 *
 * This function separates a path string (which must already be normalized and
 * must not end with a trailing '/') into two components:
 *  - The parent directory path (stored in @p parent_out).
 *  - The basename (final component of the path, stored in @p base_out).
 *
 * Rules:
 *  - If the path does not contain '/', then @p parent_out is set to an empty
 *    string (indicating "use cwd") and @p base_out is set to the full path.
 *  - If the path is "/name", then @p parent_out = "/" and @p base_out = "name".
 *  - If the path ends with '/', the function fails (invalid for a file name).
 *  - Both output buffers are NUL-terminated on success.
 *
 * Error handling:
 *  - Returns -1 if inputs are invalid, if there is no basename, or if output
 *    buffers are too small.
 *  - Returns 0 on success.
 *
 * @param normalized   Input path string (already normalized, without trailing
 * '/').
 * @param parent_out   Output buffer for the parent directory path.
 * @param parent_cap   Capacity of @p parent_out in bytes.
 * @param base_out     Output buffer for the basename (final component).
 * @param base_cap     Capacity of @p base_out in bytes.
 * @return 0 on success; -1 on error.
 */
static int split_parent_basename(const char *normalized, char *parent_out,
                                 size_t parent_cap, char *base_out,
                                 size_t base_cap) {
  if (!normalized || !*normalized)
    return -1;

  const char *last = strrchr(normalized, '/');
  if (!last) {
    // no '/', parent = "" (use cwd), base = normalized
    if (strlen(normalized) + 1 > base_cap)
      return -1;
    base_out[0] = '\0';
    strncat(base_out, normalized, base_cap - 1);
    parent_out[0] = '\0';
    return 0;
  }

  // there is '/', but could be root "/name"
  const char *base = last + 1;
  if (*base == '\0') {
    // normalized ended with '/', invalid for a file
    return -1;
  }

  size_t parent_len = (size_t)(last - normalized);
  if (parent_len == 0) {
    // parent = "/"
    if (parent_cap < 2)
      return -1;
    parent_out[0] = '/';
    parent_out[1] = '\0';
  } else {
    if (parent_len + 1 > parent_cap)
      return -1;
    memcpy(parent_out, normalized, parent_len);
    parent_out[parent_len] = '\0';
  }

  if (strlen(base) + 1 > base_cap)
    return -1;
  strncpy(base_out, base, base_cap - 1);
  base_out[base_cap - 1] = '\0';
  return 0;
}

/**
 * @brief Resolve the parent directory and basename from a path.
 *
 * This function takes an absolute or relative path, normalizes it,
 * validates it, splits it into parent directory path and basename,
 * and resolves the parent directory into a @ref dir_t structure.
 *
 * Workflow:
 *  1. Normalize the input path using normalize_path().
 *  2. Reject paths ending with '/' (files must have a basename).
 *  3. Split the normalized path into parent and basename using
 *     split_parent_basename().
 *  4. Resolve the parent path into a directory inode using
 *     lookup_file().
 *  5. Verify that the resolved parent is a directory.
 *  6. Return the parent directory in @p out_parent and the basename
 *     in @p out_basename.
 *
 * Error handling:
 *  - Returns -1 if arguments are invalid.
 *  - Returns -1 if normalization or splitting fails.
 *  - Returns -1 if the parent path cannot be resolved or is not a directory.
 *  - Returns 0 on success.
 *
 * Memory:
 *  - Internally allocates a temporary normalized path; this is freed before
 *    return.
 *
 * @param disk          Pointer to the open filesystem image.
 * @param sb            Pointer to the loaded superblock.
 * @param path          Absolute or relative path of the file.
 * @param cwd           Current working directory for relative path resolution.
 * @param out_parent    Output: resolved parent directory (as @ref dir_t).
 * @param out_basename  Output: buffer that will receive the final component
 *                      (basename) of the path.
 * @param basename_cap  Capacity of @p out_basename buffer (in bytes).
 *
 * @return 0 on success; -1 on error.
 */
int resolve_parent_dir(FILE *disk, bsfs_super_block *sb, const char *path,
                       dir_t *cwd, dir_t *out_parent, char *out_basename,
                       size_t basename_cap) {
  if (!disk || !sb || !path || !cwd || !out_parent || !out_basename ||
      basename_cap == 0)
    return -1;

  /* 1) Normalize */
  char *norm = normalize_path(path);
  if (!norm)
    return -1;

  /* Reject trailing slash */
  size_t L = strlen(norm);
  if (L > 1 && norm[L - 1] == '/') {
    free(norm);
    return -1;
  }

  /* 2) Split parent + basename */
  char parent_path[512] = {0};
  if (split_parent_basename(norm, parent_path, sizeof(parent_path),
                            out_basename, basename_cap) != 0) {
    free(norm);
    return -1;
  }
  free(norm);

  /* 3) Resolve parent */
  if (parent_path[0] == '\0') {
    /* No explicit parent → use cwd, but refresh its inode from the inode tree
       to pick up updated btree_root after splits */
    btree_handle_t *inode_handle = btree_create_inode_handle();
    inode_handle->root_block = sb->inode_root;
    bsfs_inode fresh;
    if (read_inode(disk, inode_handle, cwd->inode.inode_number, &fresh)) {
      memset(out_parent, 0, sizeof(*out_parent));
      out_parent->inode = fresh;
      out_parent->current_index = 0;
    } else {
      /* fallback to snapshot if read fails */
      *out_parent = *cwd;
    }
    free(inode_handle);
  } else {
    bsfs_inode pinode;
    if (lookup_file(disk, sb, parent_path, cwd, &pinode) != 0) {
      return -1;
    }
    if (pinode.file_type != FILE_TYPE_DIR) { /* adjust constant if needed */
      return -1;
    }

    memset(out_parent, 0, sizeof(*out_parent));
    out_parent->inode = pinode;
    out_parent->current_index = 0; /* start at first entry */
  }

  return 0;
}
