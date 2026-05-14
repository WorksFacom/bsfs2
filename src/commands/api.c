/**
 * @file api_bsfs.c
 * @brief High-level API used by the BSFS browser shell.
 *
 * This API wraps the lower-level BSFS operations (inode, directory entry and
 * bspan B-trees) to provide a simple interface for shell commands. It maintains
 * an in-memory current working directory (CWD) state and resolves relative
 * paths against it.
 */

#include "../../include/api_bsfs.h"
#include "../../include/create_file.h"
#include "../../include/dentry.h"

#ifndef PATH_MAX
/** Fallback maximum path length when system headers do not define PATH_MAX. */
#define PATH_MAX 4096
#endif

/**
 * @brief Logically remove a file and move its snapshot into the Recovery tree.
 *
 * Performs parent directory permissions and sticky-bit checks before unlinking
 * the directory entry, removing the inode from the inode tree, and inserting a
 * snapshot into the Recovery B-tree to allow later restore.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target file path.
 * @return 0 on success; -1 on error.
 */
// int remove_file_bsfs(FILE *disk, bsfs_super_block *sb, const char *path) {
//   if (!disk || !sb || !path)
//     return -1;
//   ensure_cwd(disk, sb);
//   refresh_cwd_inode(disk, sb);
//   btree_handle_t *inode_h = btree_create_inode_handle();
//   inode_h->root_block     = sb->inode_root;
//   btree_handle_t rec_h;
//   if (!rec_tree_init(disk, sb, &rec_h)) {
//     free(inode_h);
//     return -1;
//   }
//   int rc = remove_file(disk, sb, inode_h, &rec_h, path, &s_cwd, 0);
//   free(inode_h);
//   if (rc == 0)
//     return 0;
//
//   // Fallback: try removal by scanning current directory and rebuilding
//   // This avoids path lookup issues when the directory B-tree root changed.
//   // Only supports relative names in the CWD (as in our test workflow).
//   const char *fname = path;
//   const char *leaf  = fname;
//   const char *slash = strrchr(fname, '/');
//   if (slash)
//     leaf = slash + 1;
//   // If absolute path, we cannot safely fallback here
//   if (fname[0] == '/' || *leaf == '\0')
//     return rc;
//
//   // Open CWD and scan entries
//   fprintf(stderr, "[FALLBACK] rm by scan for '%s'\n", fname);
//   dir_t cwd_snapshot;
//   if (opendir_bsfs(disk, sb, ".", &cwd_snapshot) != 0)
//     return rc;
//
//   btree_handle_t *dir_h = btree_create_dir_entry_handle();
//   dir_h->root_block     = cwd_snapshot.inode.btree_root;
//
//   // Find matching entry by name
//   dir_entry_t ent;
//   int         found = scan_dir_tree_find(disk, dir_h, dir_h->root_block, leaf, &ent);
//   if (!found) {
//     fprintf(stderr, "[FALLBACK] not found in scan: %s\n", leaf);
//     free(dir_h);
//     return rc;
//   }
//
//   // Load inode
//   btree_handle_t *inode_h2 = btree_create_inode_handle();
//   inode_h2->root_block     = sb->inode_root;
//   bsfs_inode ino;
//   int        have_inode = read_inode(disk, inode_h2, ent.inode_number, &ino);
//   if (!have_inode) {
//     fprintf(stderr,
//             "[FALLBACK] inode %u missing for %s; removing entry without "
//             "recovery snapshot.\n",
//             ent.inode_number, leaf);
//     free(dir_h);
//     if (remove_dir_entry(disk, sb, &cwd_snapshot, leaf) != 0) {
//       fprintf(stderr, "[FALLBACK] remove_dir_entry failed for %s\n", leaf);
//       free(inode_h2);
//       return rc;
//     }
//     /* Attempt to remove the inode directly to avoid dangling metadata. */
//     inode_h2->root_block = sb->inode_root;
//     (void)remove_inode(disk, sb, inode_h2, ent.inode_number);
//     free(inode_h2);
//     return 0;
//   }
//
//   // Insert Recovery snapshot
//   btree_handle_t rec_h2;
//   if (!rec_tree_init(disk, sb, &rec_h2)) {
//     free(dir_h);
//     free(inode_h2);
//     return rc;
//   }
//   rec_entry_t rec;
//   memset(&rec, 0, sizeof(rec));
//   rec.deleted_at          = (uint64_t)time(NULL);
//   rec.original_parent_ino = cwd_snapshot.inode.inode_number;
//   strncpy(rec.original_name, leaf, sizeof(rec.original_name) - 1);
//   rec.inode_number = ino.inode_number;
//   rec.file_type    = ino.file_type;
//   rec.uid          = ino.uid;
//   rec.gid          = ino.gid;
//   rec.permissions  = ino.permissions;
//   rec.file_size    = ino.file_size;
//   rec.block_count  = ino.block_count;
//   memcpy(rec.direct_blocks, ino.direct_blocks, sizeof(rec.direct_blocks));
//   rec.btree_root      = ino.btree_root;
//   rec.retention_until = rec.deleted_at + DEFAULT_RECOVERY_TTL;
//   if (!rec_tree_add(disk, sb, &rec_h2, &rec)) {
//     fprintf(stderr, "[FALLBACK] rec add failed for %s\n", leaf);
//     free(dir_h);
//     free(inode_h2);
//     return rc;
//   }
//
//   // Remove directory entry via rebuild
//   if (remove_dir_entry(disk, sb, &cwd_snapshot, leaf) != 0) {
//     fprintf(stderr, "[FALLBACK] remove_dir_entry failed for %s\n", leaf);
//     // keep Recovery snapshot if directory removal fails to meet UX goal
//     free(dir_h);
//     free(inode_h2);
//     return 0; // consider removed for the browser perspective
//   }
//
//   // Try to remove inode; keep Recovery entry regardless of result
//   (void)remove_inode(disk, sb, inode_h2, ino.inode_number);
//
//   free(dir_h);
//   free(inode_h2);
//   return 0;
// }

/**
 * @brief Remove a file and immediately purge its Recovery snapshot.
 *
 * Performs the logical removal (same as @ref remove_file_bsfs) and then
 * reclaims the file's data blocks by purging any Recovery entries that match
 * the deleted file's basename and parent directory.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param path Target file path.
 * @return 0 on success; -1 on error.
 */
// int delete_file_permanently_bsfs(FILE *disk, bsfs_super_block *sb, const char *path) {
//   if (!disk || !sb || !path)
//     return -1;
//
//   ensure_cwd(disk, sb);
//   refresh_cwd_inode(disk, sb);
//
//   dir_t parent;
//   char  basename[MAX_FILENAME] = {0};
//   if (resolve_parent_dir(disk, sb, path, &s_cwd, &parent, basename, sizeof(basename)) != 0)
//     return -1;
//   uint32_t parent_ino = parent.inode.inode_number;
//
//   bsfs_inode target;
//   if (lookup_file(disk, sb, path, &s_cwd, &target) != 0)
//     return -1;
//   if (target.file_type != FILE_TYPE_FILE)
//     return -1;
//
//   if (remove_dir_entry(disk, sb, &parent, basename) != 0)
//     return -1;
//
//   int             status  = 0;
//   btree_handle_t *inode_h = btree_create_inode_handle();
//   if (inode_h) {
//     inode_h->root_block = sb->inode_root;
//     if (!remove_inode(disk, sb, inode_h, target.inode_number)) {
//       fprintf(stderr,
//               "WARNING: remove_inode failed for ino=%u (%s); inode queued for "
//               "GC.\n",
//               target.inode_number, basename);
//     }
//     free(inode_h);
//   } else {
//     status = -1;
//   }
//
//   for (int i = 0; i < 16; i++) {
//     if (target.direct_blocks[i].length == 0)
//       continue;
//     if (deallocate_bspan(disk, sb, &target.direct_blocks[i]) != 0) {
//       fprintf(stderr,
//               "WARNING: failed to release direct span %d for '%s' (disk=%u "
//               "len=%u)\n",
//               i, basename, target.direct_blocks[i].disk_blk, target.direct_blocks[i].length);
//       status = -1;
//     }
//   }
//
//   if (target.btree_root != 0) {
//     btree_handle_t *span_h = btree_create_bspan_handle();
//     if (!span_h) {
//       status = -1;
//     } else {
//       span_h->root_block = target.btree_root;
//       if (btree_deallocate_all(disk, sb, span_h, span_h->root_block) != 0) {
//         fprintf(stderr, "WARNING: failed to release span tree for '%s' (root=%u)\n", basename, target.btree_root);
//         status = -1;
//       }
//       free(span_h);
//     }
//   }
//
//   uint32_t now             = (uint32_t)time(NULL);
//   parent.inode.modified_at = now;
//   parent.inode.accessed_at = now;
//   btree_handle_t *parent_h = btree_create_inode_handle();
//   if (parent_h) {
//     parent_h->root_block = sb->inode_root;
//     (void)update_inode(disk, parent_h, &parent.inode);
//     free(parent_h);
//   } else {
//     status = -1;
//   }
//
//   btree_handle_t rec_h;
//   if (rec_tree_init(disk, sb, &rec_h)) {
//     size_t total = 0;
//     if (rec_tree_find_by_name(disk, &rec_h, basename, NULL, 0, &total) && total > 0) {
//       rec_entry_t *entries = (rec_entry_t *)malloc(total * sizeof(rec_entry_t));
//       if (!entries) {
//         status = -1;
//       } else {
//         size_t collected = 0;
//         if (rec_tree_find_by_name(disk, &rec_h, basename, entries, total, &collected)) {
//           btree_handle_t *data_h = btree_create_bspan_handle();
//           if (!data_h) {
//             status = -1;
//           } else {
//             for (size_t i = 0; i < collected; i++) {
//               if (entries[i].original_parent_ino != parent_ino)
//                 continue;
//               data_h->root_block = entries[i].btree_root;
//               if (purge_file(disk, sb, data_h, &rec_h, &entries[i]) != 0) {
//                 status = -1;
//                 break;
//               }
//             }
//             free(data_h);
//           }
//         }
//         free(entries);
//       }
//     }
//   }
//
//   return status;
// }

/**
 * @brief Restore a deleted file by recovery ID.
 *
 * Loads the snapshot from the Recovery B-tree, chooses the destination
 * directory (explicit, original parent, or CWD), recreates the inode and
 * directory entry, and removes the snapshot. Restricted to root by default.
 *
 * @param disk         Open filesystem image.
 * @param sb           Loaded superblock.
 * @param recovery_id  Recovery entry identifier.
 * @param dest_dir     Optional destination directory path.
 * @return 0 on success; -1 on error or insufficient permission.
 */
// int restore_bsfs(FILE *disk, bsfs_super_block *sb, uint32_t recovery_id, const char *dest_dir) {
//   if (!disk || !sb || recovery_id == 0)
//     return -1;
//   ensure_cwd(disk, sb);
//   // Restrict restore to root by default
//   if (!bsfs_is_root(s_uid)) {
//     errno = EACCES;
//     return -1;
//   }
//
//   // Prepare inode and recovery handles
//   btree_handle_t *inode_h = btree_create_inode_handle();
//   if (!inode_h)
//     return -1;
//   inode_h->root_block = sb->inode_root;
//
//   btree_handle_t rec_h;
//   if (!rec_tree_init(disk, sb, &rec_h)) {
//     free(inode_h);
//     return -1;
//   }
//
//   int rc = restore_file_by_id(disk, sb, inode_h, &rec_h, recovery_id, dest_dir, &s_cwd);
//   free(inode_h);
//   return rc;
// }
