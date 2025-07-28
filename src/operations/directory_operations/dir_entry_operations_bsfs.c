#include "../../include/dir_entry_operations_bsfs.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Opens a directory given its inode number.
 *
 * Loads the directory inode from the inode B-tree, initializes a dir_t
 * structure and prepares the B-tree handle for reading directory entries.
 *
 * @param disk Pointer to the open filesystem image.
 * @param inode_tree Pointer to the B-tree containing inodes.
 * @param inode_number The inode number of the directory to open.
 * @return Pointer to an allocated dir_t on success, NULL on failure.
 */
dir_t *open_dir(FILE *disk, btree_handle_t *inode_tree, uint32_t inode_number) {
  inode_t inode;
  if (!read_inode(disk, inode_tree, inode_number, &inode)) {
    fprintf(stderr, "Failed to load directory inode.\n");
    return NULL;
  }

  if (inode.file_type != 1) { // 1 = directory
    fprintf(stderr, "Inode is not a directory.\n");
    return NULL;
  }

  dir_t *dir = malloc(sizeof(dir_t));
  if (!dir) {
    fprintf(stderr, "Failed to allocate memory for dir_t.\n");
    return NULL;
  }

  dir->inode = inode;
  dir->current_index = 0;
  dir->btree = btree_create_dir_entry_handle();
  dir->btree->root_block = inode.btree_root;

  return dir;
}

/**
 * @brief Closes a directory and frees allocated resources.
 *
 * This function deallocates the memory used by the dir_t structure
 * and its associated B-tree handle.
 *
 * @param dir Pointer to the open directory to be closed.
 * @return 0 on success.
 */
int close_dir(dir_t *dir) {
  if (!dir)
    return -1;

  if (dir->btree) {
    free(dir->btree);
  }

  free(dir);
  return 0;
}

/**
 * @brief Adds a new entry to a directory.
 *
 * This function inserts a new dir_entry_t into the directory's B-tree
 * structure.
 *
 * @param disk Pointer to the filesystem image file.
 * @param sb Pointer to the loaded superblock.
 * @param dir Pointer to the open directory structure.
 * @param entry Pointer to the directory entry to insert.
 * @return 0 on success, -1 if an entry with the same name already exists.
 */
int add_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                  dir_entry_t *entry) {
  dir_entry_t existing;
  uint32_t hash = hash_filename(entry->filename);

  if (btree_search(disk, dir->btree, dir->inode.btree_root, hash, &existing)) {
    fprintf(stderr, "Directory entry '%s' already exists.\n", entry->filename);
    return -1;
  }

  btree_insert(disk, sb, dir->btree, entry);
  return 0;
}

/**
 * @brief Removes an entry from a directory by name.
 *
 * Looks up the entry in the B-tree and removes it if found.
 *
 * @param disk Pointer to the filesystem image file.
 * @param sb Pointer to the loaded superblock.
 * @param dir Pointer to the open directory structure.
 * @param filename Name of the entry to remove.
 * @return 0 on success, -1 if the entry was not found.
 */
int remove_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                     const char *filename) {
  uint32_t hash = hash_filename(filename);

  dir_entry_t found;
  if (!btree_search(disk, dir->btree, dir->inode.btree_root, hash, &found)) {
    fprintf(stderr, "Directory entry '%s' not found.\n", filename);
    return -1;
  }

  return btree_remove(disk, sb, dir->btree, hash);
}

/**
 * @brief Updates an existing directory entry in the B-tree.
 *
 * This function searches for an existing directory entry using the hash of the
 * filename and replaces its contents with the new data.
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param dir Pointer to the open directory structure.
 * @param entry Pointer to the updated directory entry.
 * @return 0 on success, -1 if the entry was not found.
 */
int update_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                     dir_entry_t *entry) {
  uint32_t hash = hash_filename(entry->filename);

  dir_entry_t existing;
  if (!btree_search(disk, dir->btree, dir->inode.btree_root, hash, &existing)) {
    fprintf(stderr, "Cannot update: entry '%s' not found in directory.\n",
            entry->filename);
    return -1;
  }

  return btree_update(disk, dir->btree, dir->inode.btree_root, entry);
}

/**
 * @brief Looks up a directory entry by name in a given directory.
 *
 * @param disk Pointer to the disk image.
 * @param sb Pointer to the superblock.
 * @param dir Pointer to the open directory (dir_t).
 * @param name Name of the entry to look for.
 * @param result Output parameter: filled with the entry if found.
 * @return 0 if found, -1 if not found.
 */
int lookup_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir, const char *name,
                     dir_entry_t *result) {
  btree_handle_t *handle = btree_create_dir_entry_handle();
  handle->root_block = dir->inode.btree_root;

  dir_entry_t temp;
  strncpy(temp.filename, name, MAX_FILENAME);
  temp.filename[MAX_FILENAME - 1] = '\0';

  uint32_t key = hash_filename(temp.filename);

  int found = btree_search(disk, handle, handle->root_block, key, result);
  free(handle);
  return found ? 0 : -1;
}

/**
 * @brief Reads the next directory entry from a directory.
 *
 * This function traverses the B-tree of a directory and returns the next valid
 * entry. It uses the `dir_index` field to keep track of position across calls.
 *
 * @param disk Pointer to the open disk file.
 * @param sb Pointer to the superblock.
 * @param dir Pointer to the open directory structure (dir_t).
 * @param handle B-tree handler for the directory entries.
 * @param out Output parameter to hold the directory entry read.
 * @return 0 if an entry was read successfully, -1 if end of directory is
 * reached.
 */
int read_dir_entry(FILE *disk, superblock_t *sb, dir_t *dir,
                   btree_handle_t *handle, dir_entry_t *out) {
  if (!disk || !sb || !dir || !handle || !out)
    return -1;

  while (dir->current_index <= 0xFFFFFFFF) {
    uint32_t hash = dir->current_index++;
    if (btree_search(disk, handle, dir->inode.btree_root, hash, out)) {
      return 0;
    }
  }

  return -1;
}
