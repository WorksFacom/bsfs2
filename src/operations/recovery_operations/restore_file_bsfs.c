/**
 * @file restore_file_bsfs.c
 * @brief Implementation of file restoration from the Recovery tree in BSFS.
 *
 * This module provides the routines necessary to restore a deleted file using
 * its snapshot stored in the Recovery B-tree. Restoration consists of
 * recreating the inode (reusing recorded data block mappings), linking a new
 * directory entry in the chosen destination directory, and removing the
 * snapshot from the Recovery tree upon success.
 */

#include "../../include/restore_file_bsfs.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/**
 * @brief Determine a destination directory for restoration.
 *
 * Resolution order:
 *  - If @p dest_dir_path is non-NULL, resolve it (relative to @p cwd if
 *    needed) and ensure it is a directory.
 *  - Else if the snapshot's original parent inode exists and is a directory,
 *    use it.
 *  - Else fall back to @p cwd.
 *
 * @param disk          Open filesystem image.
 * @param sb            Loaded superblock.
 * @param inode_tree    Inode B-tree handle.
 * @param rec           Recovery snapshot providing original parent.
 * @param dest_dir_path Optional path to destination directory.
 * @param cwd           Current working directory (fallback).
 * @param out_dir       Output: resolved destination directory descriptor.
 * @return 0 on success; -1 on failure.
 */
static int resolve_destination_dir(FILE *disk, superblock_t *sb,
                                   btree_handle_t *inode_tree,
                                   const rec_entry_t *rec,
                                   const char *dest_dir_path, dir_t *cwd,
                                   dir_t *out_dir) {
  inode_t dir_inode;

  if (dest_dir_path && dest_dir_path[0] != '\0') {
    if (lookup_file(disk, sb, dest_dir_path, cwd, &dir_inode) != 0)
      return -1;
    if (dir_inode.file_type != FILE_TYPE_DIR)
      return -1;
    out_dir->inode = dir_inode;
    out_dir->current_index = 0;
    return 0;
  }

  if (rec && rec->original_parent_ino != 0) {
    if (read_inode(disk, inode_tree, rec->original_parent_ino, &dir_inode)) {
      if (dir_inode.file_type == FILE_TYPE_DIR) {
        out_dir->inode = dir_inode;
        out_dir->current_index = 0;
        return 0;
      }
    }
  }

  if (cwd) {
    *out_dir = *cwd;
    return 0;
  }

  return -1;
}

/**
 * @brief Generate a unique filename under a destination directory.
 *
 * If @p desired already exists in @p dest_dir, appends a suffix based on the
 * @p recovery_id (e.g., "name(recovered-<id>)"). If the name is still taken or
 * too long, progressively tries compact fallback names like
 * "recovered-<id>" and "recovered-<id>-<n>".
 *
 * @param disk         Open filesystem image.
 * @param sb           Loaded superblock.
 * @param dest_dir     Destination directory descriptor.
 * @param desired      Desired filename.
 * @param recovery_id  Recovery entry ID used in suffixes.
 * @param out_name     Output buffer for the chosen name.
 * @param out_cap      Capacity of @p out_name.
 * @return 0 if a unique name was produced; -1 on error.
 */
static int make_unique_name(FILE *disk, superblock_t *sb, dir_t *dest_dir,
                            const char *desired, uint32_t recovery_id,
                            char *out_name, size_t out_cap) {
  if (!desired || !*desired || !out_name || out_cap == 0)
    return -1;

  dir_entry_t tmp;
  // First try desired as-is
  strncpy(out_name, desired, out_cap - 1);
  out_name[out_cap - 1] = '\0';
  if (lookup_dir_entry(disk, sb, dest_dir, out_name, &tmp) != 0) {
    return 0; // not found, ok
  }

  // Try "<desired>(recovered-<id>)"
  int written =
      snprintf(out_name, out_cap, "%s(recovered-%u)", desired, recovery_id);
  if (written > 0 && (size_t)written < out_cap) {
    if (lookup_dir_entry(disk, sb, dest_dir, out_name, &tmp) != 0) {
      return 0;
    }
  }

  // Fallback to a shorter base: "recovered-<id>" and then add a counter
  written = snprintf(out_name, out_cap, "recovered-%u", recovery_id);
  if (!(written > 0 && (size_t)written < out_cap))
    return -1;
  if (lookup_dir_entry(disk, sb, dest_dir, out_name, &tmp) != 0) {
    return 0;
  }

  for (unsigned n = 2; n < 1000; n++) {
    written = snprintf(out_name, out_cap, "recovered-%u-%u", recovery_id, n);
    if (!(written > 0 && (size_t)written < out_cap))
      return -1;
    if (lookup_dir_entry(disk, sb, dest_dir, out_name, &tmp) != 0) {
      return 0;
    }
  }

  return -1;
}

/**
 * @brief Recreate an inode from a Recovery snapshot, choosing an inode number.
 *
 * Prefers reusing the inode number recorded in the snapshot if available;
 * otherwise obtains a fresh inode number from the inode tree.
 *
 * @param disk        Open filesystem image.
 * @param inode_tree  Inode B-tree handle.
 * @param rec         Recovery snapshot.
 * @param out_inode   Output: constructed inode.
 * @return 0 on success; -1 on failure.
 */
static int build_inode_from_snapshot(FILE *disk, btree_handle_t *inode_tree,
                                     const rec_entry_t *rec,
                                     inode_t *out_inode) {
  if (!disk || !inode_tree || !rec || !out_inode)
    return -1;

  memset(out_inode, 0, sizeof(*out_inode));

  uint32_t chosen_ino = rec->inode_number;
  if (!chosen_ino || inode_exists(disk, inode_tree, chosen_ino)) {
    chosen_ino = get_next_inode_number(disk, inode_tree);
    if (!chosen_ino)
      return -1;
  }

  out_inode->inode_number = chosen_ino;
  out_inode->file_type = rec->file_type;
  out_inode->uid = rec->uid;
  out_inode->gid = rec->gid;
  out_inode->permissions = rec->permissions;
  out_inode->file_size = rec->file_size;
  out_inode->block_count = rec->block_count;

  for (int i = 0; i < 16; i++)
    out_inode->direct_blocks[i] = rec->direct_blocks[i];
  out_inode->btree_root = rec->btree_root;

  uint32_t now = (uint32_t)time(NULL);
  out_inode->created_at = now;
  out_inode->modified_at = now;
  out_inode->accessed_at = now;
  out_inode->links_count = 1; // will be linked into the destination dir

  return 0;
}

int restore_file_by_id(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                       btree_handle_t *rec_tree, uint32_t recovery_id,
                       const char *dest_dir_path, dir_t *cwd) {
  if (!disk || !sb || !inode_tree || !rec_tree)
    return -1;

  // 1) Lookup snapshot in Recovery tree
  rec_entry_t rec;
  if (!rec_tree_search(disk, rec_tree, recovery_id, &rec))
    return -1;

  // 2) Determine destination directory
  dir_t dest_dir;
  if (resolve_destination_dir(disk, sb, inode_tree, &rec, dest_dir_path, cwd,
                              &dest_dir) != 0)
    return -1;

  // 3) Resolve unique filename in destination
  char final_name[MAX_FILENAME];
  if (make_unique_name(disk, sb, &dest_dir, rec.original_name, recovery_id,
                       final_name, sizeof(final_name)) != 0)
    return -1;

  // 4) Build inode from snapshot (choose inode number if needed)
  inode_t inode;
  if (build_inode_from_snapshot(disk, inode_tree, &rec, &inode) != 0)
    return -1;

  // 5) Insert inode into inode tree (persist root if created)
  uint32_t prev_root = inode_tree->root_block;
  if (insert_inode(disk, sb, inode_tree, &inode) != 0)
    return -1;
  if (sb->inode_root == 0 && inode_tree->root_block != 0 && prev_root == 0) {
    sb->inode_root = inode_tree->root_block;
    (void)write_superblock(disk, sb);
  }

  // 6) Link directory entry in destination
  dir_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.inode_number = inode.inode_number;
  entry.file_type = inode.file_type;
  entry.file_size = 0; // optional cached size, if used elsewhere
  strncpy(entry.filename, final_name, sizeof(entry.filename) - 1);
  entry.filename[sizeof(entry.filename) - 1] = '\0';

  if (add_dir_entry(disk, sb, &dest_dir, &entry) != 0) {
    // Rollback: remove inserted inode
    (void)remove_inode(disk, sb, inode_tree, inode.inode_number);
    return -1;
  }

  // Update directory timestamps
  dest_dir.inode.modified_at = (uint32_t)time(NULL);
  dest_dir.inode.accessed_at = dest_dir.inode.modified_at;
  (void)update_inode(disk, inode_tree, &dest_dir.inode);

  // 7) Remove snapshot from Recovery tree
  if (!rec_tree_remove(disk, sb, rec_tree, recovery_id)) {
    // File is restored but snapshot remains; report soft failure
    return -1;
  }

  return 0;
}
