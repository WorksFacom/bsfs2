/**
 * @file api_bsfs.c
 * @brief High-level API used by the BSFS browser shell.
 *
 * This API wraps the lower-level BSFS operations (inode, directory entry and
 * bspan B-trees) to provide a simple interface for shell commands. It maintains
 * an in-memory current working directory (CWD) state and resolves relative
 * paths against it.
 */

#include "../include/api_bsfs.h"

#include "../include/btree_bsfs.h"
#include "../include/create_file_bsfs.h"
#include "../include/dir_entry_operations_bsfs.h"
#include "../include/directory_operations_bsfs.h"
#include "../include/file_operations_bsfs.h"
#include "../include/inode_operations_bsfs.h"
#include "../include/path_utils_bsfs.h"
#include "../include/rec_entry_operations_bsfs.h"
#include "../include/remove_file_bsfs.h"
#include "../include/restore_file_bsfs.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int s_cwd_initialized = 0;
static dir_t s_cwd;
static char s_cwd_path[PATH_MAX] = "/";
static uint16_t s_uid = 0, s_gid = 0; // session identity (root by default)
static uint16_t s_umask = 0022;       // default umask

static void ensure_cwd(FILE *disk, superblock_t *sb) {
  if (s_cwd_initialized)
    return;
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  inode_t root;
  if (!read_inode(disk, inode_h, sb->root_inode, &root)) {
    // fallback: zero init
    memset(&s_cwd, 0, sizeof(s_cwd));
    s_cwd_initialized = 1;
    s_cwd_path[0] = '/';
    s_cwd_path[1] = '\0';
    free(inode_h);
    return;
  }
  memset(&s_cwd, 0, sizeof(s_cwd));
  s_cwd.inode = root;
  s_cwd.current_index = 0;
  s_cwd_initialized = 1;
  s_cwd_path[0] = '/';
  s_cwd_path[1] = '\0';
  free(inode_h);
}

// Text-only path adjuster similar to cd.c
static void cwd_update_text(const char *target_path) {
  if (!target_path || !*target_path)
    return;
  if (target_path[0] == '/') {
    // absolute
    strncpy(s_cwd_path, target_path, PATH_MAX - 1);
    s_cwd_path[PATH_MAX - 1] = '\0';
    return;
  }
  if (strcmp(target_path, ".") == 0)
    return;
  if (strcmp(target_path, "..") == 0) {
    char *last_slash = strrchr(s_cwd_path, '/');
    if (last_slash && last_slash != s_cwd_path) {
      *last_slash = '\0';
    } else {
      strcpy(s_cwd_path, "/");
    }
    return;
  }
  size_t L = strlen(s_cwd_path);
  if (L > 1 && s_cwd_path[L - 1] != '/') {
    strncat(s_cwd_path, "/", PATH_MAX - strlen(s_cwd_path) - 1);
  }
  strncat(s_cwd_path, target_path, PATH_MAX - strlen(s_cwd_path) - 1);
}

int open_bsfs(FILE *disk, superblock_t *sb, const char *path, file_t *file) {
  if (!disk || !sb || !path || !file)
    return -1;
  ensure_cwd(disk, sb);
  inode_t inode;
  if (lookup_file(disk, sb, path, &s_cwd, &inode) != 0)
    return -1;
  // For now, just load; read/write checks are enforced in read_bsfs/write_bsfs
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  int rc = open_file(disk, inode_h, inode.inode_number, file);
  free(inode_h);
  return rc;
}

int close_bsfs(file_t *file) {
  (void)file; // write_bsfs persists inode; close is a no-op in this API
  return 0;
}

int seek_bsfs(file_t *file, uint32_t pos) {
  if (!file)
    return -1;
  if (pos > file->inode.file_size)
    return -1;
  file->pos = pos;
  return 0;
}

size_t read_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                 uint32_t length) {
  (void)sb;
  if (!disk || !file || !data || length == 0)
    return 0;
  if (!bsfs_can_read_file(&file->inode, s_uid, s_gid)) {
    errno = EACCES;
    return 0;
  }
  btree_handle_t *bspan_h = btree_create_bspan_handle();
  bspan_h->root_block = file->inode.btree_root;
  int n = read_file(disk, bspan_h, file, (uint8_t *)data, length);
  free(bspan_h);
  return (n < 0) ? 0 : (size_t)n;
}

size_t write_bsfs(FILE *disk, superblock_t *sb, file_t *file, void *data,
                  uint32_t length) {
  if (!disk || !sb || !file || !data || length == 0)
    return 0;
  if (!bsfs_can_write_file(&file->inode, s_uid, s_gid)) {
    errno = EACCES;
    return 0;
  }
  btree_handle_t *bspan_h = btree_create_bspan_handle();
  bspan_h->root_block = file->inode.btree_root;
  int n = write_file(disk, sb, bspan_h, file, (const uint8_t *)data, length, 0);
  free(bspan_h);
  if (n < 0)
    return 0;
  // Persist inode metadata update
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  (void)update_inode(disk, inode_h, &file->inode);
  free(inode_h);
  return (size_t)n;
}

size_t overwrite_bsfs(FILE *disk, superblock_t *sb, const char *path,
                      const void *data, uint32_t length) {
  if (!disk || !sb || !path || !data || length == 0)
    return 0;
  ensure_cwd(disk, sb);

  // Open the target file
  file_t f = {0};
  if (open_bsfs(disk, sb, path, &f) != 0) {
    return 0;
  }
  // Permission check is done in write_bsfs, but we need to ensure truncate
  btree_handle_t *bspan_h = btree_create_bspan_handle();
  bspan_h->root_block = f.inode.btree_root;
  // Write from pos=0 with truncate
  f.pos = 0;
  int n = write_file(disk, sb, bspan_h, &f, (const uint8_t *)data, length, 1);
  free(bspan_h);
  if (n < 0)
    return 0;
  // Persist inode metadata update
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  (void)update_inode(disk, inode_h, &f.inode);
  free(inode_h);
  return (size_t)n;
}

int create_file_bsfs(FILE *disk, superblock_t *sb, const char *path) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  // Check parent directory permissions
  dir_t parent;
  char name[MAX_FILENAME] = {0};
  if (resolve_parent_dir(disk, sb, path, &s_cwd, &parent, name, sizeof(name)) !=
      0)
    return -1;
  if (!bsfs_can_modify_dir(&parent.inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  uint16_t mode = bsfs_apply_umask(0644, s_umask);
  int rc = create_file(disk, sb, inode_h, path, &s_cwd, s_uid, s_gid, mode);
  free(inode_h);
  return rc;
}

int remove_file_bsfs(FILE *disk, superblock_t *sb, const char *path) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  // Pre-check permissions on parent and entry (sticky respected)
  dir_t parent;
  char name[MAX_FILENAME] = {0};
  if (resolve_parent_dir(disk, sb, path, &s_cwd, &parent, name, sizeof(name)) !=
      0)
    return -1;
  if (!bsfs_can_modify_dir(&parent.inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  dir_entry_t entry;
  if (lookup_dir_entry(disk, sb, &parent, name, &entry) != 0)
    return -1;
  btree_handle_t *inode_h_tmp = btree_create_inode_handle();
  inode_h_tmp->root_block = sb->inode_root;
  inode_t ent_inode;
  if (!read_inode(disk, inode_h_tmp, entry.inode_number, &ent_inode)) {
    free(inode_h_tmp);
    return -1;
  }
  free(inode_h_tmp);
  if (!bsfs_can_unlink_in_sticky(&parent.inode, &ent_inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h)) {
    free(inode_h);
    return -1;
  }
  int rc = remove_file(disk, sb, inode_h, &rec_h, path, &s_cwd, 0);
  free(inode_h);
  return rc;
}

int move_bsfs(FILE *disk, superblock_t *sb, const char *old_path,
              const char *new_path) {
  if (!disk || !sb || !old_path || !new_path)
    return -1;
  ensure_cwd(disk, sb);

  // Resolve source parent and name
  dir_t src_parent;
  char src_name[MAX_FILENAME] = {0};
  if (resolve_parent_dir(disk, sb, old_path, &s_cwd, &src_parent, src_name,
                         sizeof(src_name)) != 0)
    return -1;

  // Resolve destination parent and final name
  dir_t dest_parent;
  char dest_name[MAX_FILENAME] = {0};
  if (resolve_parent_dir(disk, sb, new_path, &s_cwd, &dest_parent, dest_name,
                         sizeof(dest_name)) != 0)
    return -1;

  // Permission checks: modify on both dirs; sticky on source
  if (!bsfs_can_modify_dir(&src_parent.inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  if (!bsfs_can_modify_dir(&dest_parent.inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  dir_entry_t src_entry;
  if (lookup_dir_entry(disk, sb, &src_parent, src_name, &src_entry) != 0)
    return -1;
  btree_handle_t *inode_h_tmp2 = btree_create_inode_handle();
  inode_h_tmp2->root_block = sb->inode_root;
  inode_t src_inode;
  if (!read_inode(disk, inode_h_tmp2, src_entry.inode_number, &src_inode)) {
    free(inode_h_tmp2);
    return -1;
  }
  free(inode_h_tmp2);
  if (!bsfs_can_unlink_in_sticky(&src_parent.inode, &src_inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }

  return move_dir_entry(disk, sb, &src_parent, src_name, &dest_parent,
                        dest_name);
}

int stat_bsfs(FILE *disk, superblock_t *sb, const char *path, inode_t *inode) {
  if (!disk || !sb || !path || !inode)
    return -1;
  ensure_cwd(disk, sb);
  return lookup_file(disk, sb, path, &s_cwd, inode);
}

int opendir_bsfs(FILE *disk, superblock_t *sb, const char *dirname,
                 dir_t *dir) {
  if (!disk || !sb || !dir)
    return -1;
  ensure_cwd(disk, sb);
  if (!dirname || strcmp(dirname, ".") == 0) {
    *dir = s_cwd;
    if (!bsfs_can_traverse_dir(&dir->inode, s_uid, s_gid) ||
        !bsfs_can_list_dir(&dir->inode, s_uid, s_gid)) {
      errno = EACCES;
      return -1;
    }
    dir->current_index = 0;
    return 0;
  }
  inode_t ino;
  if (lookup_file(disk, sb, dirname, &s_cwd, &ino) != 0)
    return -1;
  if (ino.file_type != FILE_TYPE_DIR)
    return -1;
  if (!bsfs_can_traverse_dir(&ino, s_uid, s_gid) ||
      !bsfs_can_list_dir(&ino, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  memset(dir, 0, sizeof(*dir));
  dir->inode = ino;
  dir->current_index = 0;
  return 0;
}

int closedir_bsfs(dir_t *dir) {
  (void)dir;
  return 0;
}

int readdir_bsfs(FILE *disk, superblock_t *sb, dir_t *dir, dir_entry_t *entry) {
  if (!disk || !sb || !dir || !entry)
    return -1;
  // The underlying read_dir_entry returns 0 on success, -1 on end/error
  int rc = read_dir_entry(disk, sb, dir, NULL, entry);
  if (rc == 0)
    return 1; // success
  return 0;   // end
}

int mkdir_bsfs(FILE *disk, superblock_t *sb, const char *path) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  // Check parent permissions
  dir_t parent;
  char name[MAX_FILENAME] = {0};
  if (resolve_parent_dir(disk, sb, path, &s_cwd, &parent, name, sizeof(name)) !=
      0)
    return -1;
  if (!bsfs_can_modify_dir(&parent.inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  int rc = create_directory(disk, sb, path, &s_cwd);
  if (rc != 0)
    return rc;
  // Post-adjust owner/mode with umask.
  dir_entry_t created;
  if (lookup_dir_entry(disk, sb, &parent, name, &created) == 0) {
    btree_handle_t *inode_h = btree_create_inode_handle();
    inode_h->root_block = sb->inode_root;
    inode_t ino;
    if (read_inode(disk, inode_h, created.inode_number, &ino)) {
      ino.uid = s_uid;
      ino.gid = s_gid;
      ino.permissions = bsfs_apply_umask(0755, s_umask);
      (void)update_inode(disk, inode_h, &ino);
    }
    free(inode_h);
  }
  return 0;
}

int rmdir_bsfs(FILE *disk, superblock_t *sb, const char *path) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  // Check parent + sticky with target
  dir_t parent;
  char name[MAX_FILENAME] = {0};
  if (resolve_parent_dir(disk, sb, path, &s_cwd, &parent, name, sizeof(name)) !=
      0)
    return -1;
  if (!bsfs_can_modify_dir(&parent.inode, s_uid, s_gid))
    return -1;
  dir_entry_t entry;
  if (lookup_dir_entry(disk, sb, &parent, name, &entry) != 0)
    return -1;
  btree_handle_t *inode_h_tmp3 = btree_create_inode_handle();
  inode_h_tmp3->root_block = sb->inode_root;
  inode_t dir_inode;
  if (!read_inode(disk, inode_h_tmp3, entry.inode_number, &dir_inode)) {
    free(inode_h_tmp3);
    return -1;
  }
  free(inode_h_tmp3);
  if (!bsfs_can_unlink_in_sticky(&parent.inode, &dir_inode, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  return remove_directory(disk, sb, path, &s_cwd);
}

int chdir_bsfs(FILE *disk, superblock_t *sb, const char *path) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  inode_t ino;
  if (lookup_file(disk, sb, path, &s_cwd, &ino) != 0)
    return -1;
  if (ino.file_type != FILE_TYPE_DIR)
    return -1;
  if (!bsfs_can_traverse_dir(&ino, s_uid, s_gid)) {
    errno = EACCES;
    return -1;
  }
  s_cwd.inode = ino;
  s_cwd.current_index = 0;
  cwd_update_text(path);
  return 0;
}

int getcwd_bsfs(FILE *disk, superblock_t *sb, char *path_buffer,
                size_t buffer_size) {
  (void)disk;
  (void)sb;
  if (!path_buffer || buffer_size == 0)
    return -1;
  strncpy(path_buffer, s_cwd_path, buffer_size - 1);
  path_buffer[buffer_size - 1] = '\0';
  return 0;
}

int restore_bsfs(FILE *disk, superblock_t *sb, uint32_t recovery_id,
                 const char *dest_dir) {
  if (!disk || !sb || recovery_id == 0)
    return -1;
  ensure_cwd(disk, sb);
  // Restrict restore to root by default
  if (!bsfs_is_root(s_uid)) {
    errno = EACCES;
    return -1;
  }

  // Prepare inode and recovery handles
  btree_handle_t *inode_h = btree_create_inode_handle();
  if (!inode_h)
    return -1;
  inode_h->root_block = sb->inode_root;

  btree_handle_t rec_h;
  if (!rec_tree_init(disk, sb, &rec_h)) {
    free(inode_h);
    return -1;
  }

  int rc = restore_file_by_id(disk, sb, inode_h, &rec_h, recovery_id, dest_dir,
                              &s_cwd);
  free(inode_h);
  return rc;
}

int chmod_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t mode) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  inode_t ino;
  if (lookup_file(disk, sb, path, &s_cwd, &ino) != 0)
    return -1;
  if (!(bsfs_is_root(s_uid) || s_uid == ino.uid)) {
    errno = EACCES;
    return -1;
  }
  ino.permissions = (uint16_t)(mode & 07777);
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  int ok = update_inode(disk, inode_h, &ino);
  free(inode_h);
  return ok ? 0 : -1;
}

int chown_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t uid) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  inode_t ino;
  if (lookup_file(disk, sb, path, &s_cwd, &ino) != 0)
    return -1;
  if (!bsfs_is_root(s_uid)) {
    errno = EACCES;
    return -1;
  }
  ino.uid = uid;
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  int ok = update_inode(disk, inode_h, &ino);
  free(inode_h);
  return ok ? 0 : -1;
}

int chgrp_bsfs(FILE *disk, superblock_t *sb, const char *path, uint16_t gid) {
  if (!disk || !sb || !path)
    return -1;
  ensure_cwd(disk, sb);
  inode_t ino;
  if (lookup_file(disk, sb, path, &s_cwd, &ino) != 0)
    return -1;
  if (!bsfs_is_root(s_uid)) {
    errno = EACCES;
    return -1;
  }
  ino.gid = gid;
  btree_handle_t *inode_h = btree_create_inode_handle();
  inode_h->root_block = sb->inode_root;
  int ok = update_inode(disk, inode_h, &ino);
  free(inode_h);
  return ok ? 0 : -1;
}

int set_identity_bsfs(uint16_t uid, uint16_t gid) {
  s_uid = uid;
  s_gid = gid;
  return 0;
}

int get_identity_bsfs(uint16_t *uid, uint16_t *gid) {
  if (uid)
    *uid = s_uid;
  if (gid)
    *gid = s_gid;
  return 0;
}

int set_umask_bsfs(uint16_t umask) {
  s_umask = (uint16_t)(umask & 0777);
  return 0;
}

int get_umask_bsfs(uint16_t *umask) {
  if (umask)
    *umask = s_umask;
  return 0;
}
