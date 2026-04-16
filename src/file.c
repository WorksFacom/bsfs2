/**
 * @file file_operations_bsfs.c
 * @brief Implementation of file-level operations in the BSFS file system.
 *
 * This file provides functions to manipulate files, including opening,
 * closing, reading, writing, and seeking, by utilizing inodes and
 * B-tree-based address mapping. Files are loaded into memory as `file_t`
 * structures which manage stateful access.
 */
#include "../include/file.h"

// static int bspan_search_le(FILE *disk, btree_handle_t *handle, uint32_t key, bspan_t *out) {
//   if (!handle || handle->root_block == 0)
//     return 0;
//   uint32_t node_block = handle->root_block;
//   bspan_t  best       = {0};
//   int      have       = 0;
//
//   while (node_block) {
//     btree_node_t *node = btree_read_node(disk, handle, node_block);
//     if (!node)
//       break;
//     uint8_t *entries = node->entries;
//     uint32_t i       = 0;
//     // find first entry with key >= target
//     while (i < node->num_entries) {
//       uint32_t ekey = handle->get_key(entries + i * handle->entry_size);
//       if (ekey > key)
//         break;
//       // this entry is <= key; candidate predecessor
//       memcpy(&best, entries + i * handle->entry_size, sizeof(best));
//       have = 1;
//       i++;
//     }
//     if (node->is_leaf) {
//       btree_free_node(node);
//       break;
//     }
//     // descend to child i
//     uint32_t child = node->children[i];
//     btree_free_node(node);
//     node_block = child;
//   }
//   if (have) {
//     *out = best;
//     return 1;
//   }
//   return 0;
// }
