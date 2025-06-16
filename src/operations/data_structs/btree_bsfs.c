#include "../../include/btree_bsfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Funções auxiliares ===
static int default_compare(const void *a, const void *b) {
  uint32_t ka = *(uint32_t *)a;
  uint32_t kb = *(uint32_t *)b;
  return (ka < kb) ? -1 : (ka > kb);
}

static int compute_min_degree(size_t entry_size) {
  size_t entry_area = entry_size * (2 * 4 - 1);
  size_t pointer_area = sizeof(uint32_t) * (2 * 4);
  int t = 2;
  while (1) {
    entry_area = entry_size * (2 * t - 1);
    pointer_area = sizeof(uint32_t) * (2 * t);
    size_t node_overhead = sizeof(uint8_t) + sizeof(uint32_t);
    if (entry_area + pointer_area + node_overhead > BLOCK_SIZE) {
      return t - 1;
    }
    t++;
  }
}

static void btree_shift_left(void *entries, size_t entry_size, int from,
                             int count) {
  for (int i = from; i < count - 1; i++) {
    memcpy((uint8_t *)entries + i * entry_size,
           (uint8_t *)entries + (i + 1) * entry_size, entry_size);
  }
}

// === Manipuladores específicos ===
uint32_t get_inode_key(const void *entry) {
  return ((const inode_t *)entry)->inode_number;
}

int compare_inode(const void *a, const void *b) {
  return (get_inode_key(a) > get_inode_key(b)) -
         (get_inode_key(a) < get_inode_key(b));
}

btree_handle_t *btree_create_inode_handle() {
  return btree_create_handle(BTREE_TYPE_INODE, sizeof(inode_t), compare_inode,
                             get_inode_key);
}

uint32_t get_bspan_key(const void *entry) {
  return ((const bspan_t *)entry)->file_blk;
}

int compare_bspan(const void *a, const void *b) {
  return (get_bspan_key(a) > get_bspan_key(b)) -
         (get_bspan_key(a) < get_bspan_key(b));
}

btree_handle_t *btree_create_bspan_handle() {
  return btree_create_handle(BTREE_TYPE_BSPAN, sizeof(bspan_t), compare_bspan,
                             get_bspan_key);
}

uint32_t get_rec_entry_key(const void *entry) {
  return ((const rec_entry_t *)entry)->index;
}

int compare_rec_entry(const void *a, const void *b) {
  return (get_rec_entry_key(a) > get_rec_entry_key(b)) -
         (get_rec_entry_key(a) < get_rec_entry_key(b));
}

btree_handle_t *btree_create_rec_entry_handle() {
  return btree_create_handle(BTREE_TYPE_RECOVERY, sizeof(rec_entry_t),
                             compare_rec_entry, get_rec_entry_key);
}

// === Inicialização do manipulador de árvore ===
btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size,
                                    compare_fn cmp, key_fn get_key) {
  btree_handle_t *handle = malloc(sizeof(btree_handle_t));
  handle->type = type;
  handle->entry_size = entry_size;
  handle->cmp = cmp ? cmp : default_compare;
  handle->get_key = get_key;
  handle->root_block = 0;
  handle->min_degree = compute_min_degree(entry_size);
  return handle;
}

// === Atualização de entrada na árvore ===
int btree_update(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 void *new_entry) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  uint8_t *entries = node->entries;
  uint32_t key = handle->get_key(new_entry);

  int i = 0;
  while (i < node->num_entries &&
         handle->cmp(&key, entries + i * handle->entry_size) > 0) {
    i++;
  }

  if (i < node->num_entries &&
      handle->cmp(&key, entries + i * handle->entry_size) == 0) {
    memcpy(entries + i * handle->entry_size, new_entry, handle->entry_size);
    btree_write_node(disk, handle, node_block, node);
    btree_free_node(node);
    return 1;
  }

  if (node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  int updated = btree_update(disk, handle, node->children[i], new_entry);
  if (updated) {
    btree_write_node(disk, handle, node_block, node);
  }
  btree_free_node(node);
  return updated;
}

// === Escrita e leitura de nós ===
void btree_write_node(FILE *disk, btree_handle_t *handle, uint32_t block,
                      btree_node_t *node) {
  uint8_t buffer[BLOCK_SIZE] = {0};
  size_t offset = 0;
  memcpy(buffer + offset, &node->is_leaf, sizeof(uint8_t));
  offset += sizeof(uint8_t);
  memcpy(buffer + offset, &node->num_entries, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  memcpy(buffer + offset, node->entries,
         handle->entry_size * (2 * handle->min_degree - 1));
  offset += handle->entry_size * (2 * handle->min_degree - 1);
  memcpy(buffer + offset, node->children,
         sizeof(uint32_t) * (2 * handle->min_degree));
  write_block(disk, block, buffer, sizeof(buffer));
}

btree_node_t *btree_read_node(FILE *disk, btree_handle_t *handle,
                              uint32_t block) {
  uint8_t buffer[BLOCK_SIZE];
  read_block(disk, block, buffer, sizeof(buffer));
  size_t offset = 0;
  btree_node_t *node = malloc(sizeof(btree_node_t));
  memcpy(&node->is_leaf, buffer + offset, sizeof(uint8_t));
  offset += sizeof(uint8_t);
  memcpy(&node->num_entries, buffer + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  node->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
  memcpy(node->entries, buffer + offset,
         handle->entry_size * (2 * handle->min_degree - 1));
  offset += handle->entry_size * (2 * handle->min_degree - 1);
  node->children = calloc(2 * handle->min_degree, sizeof(uint32_t));
  memcpy(node->children, buffer + offset,
         sizeof(uint32_t) * (2 * handle->min_degree));
  return node;
}

void btree_free_node(btree_node_t *node) {
  free(node->entries);
  free(node->children);
  free(node);
}

// === Busca genérica ===
int btree_search(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 uint32_t key, void *output) {
  if (node_block == 0)
    return 0;
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  uint8_t *entries = node->entries;

  int i = 0;
  while (i < node->num_entries &&
         handle->cmp(&key, entries + i * handle->entry_size) > 0) {
    i++;
  }

  if (i < node->num_entries &&
      handle->cmp(&key, entries + i * handle->entry_size) == 0) {
    memcpy(output, entries + i * handle->entry_size, handle->entry_size);
    btree_free_node(node);
    return 1;
  }

  if (node->is_leaf) {
    btree_free_node(node);
    return 0;
  }

  uint32_t child_block = node->children[i];
  btree_free_node(node);
  return btree_search(disk, handle, child_block, key, output);
}

// === Inserção ===
static void btree_insert_nonfull(FILE *disk, superblock_t *sb,
                                 btree_handle_t *handle, btree_node_t *node,
                                 void *entry);
static void btree_split_child(FILE *disk, superblock_t *sb,
                              btree_handle_t *handle, btree_node_t *parent,
                              int index);

void btree_insert(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                  void *entry) {
  if (handle->root_block == 0) {
    btree_node_t *root = malloc(sizeof(btree_node_t));
    root->is_leaf = 1;
    root->num_entries = 0;
    root->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
    root->children = calloc(2 * handle->min_degree, sizeof(uint32_t));

    handle->root_block = allocate_single_block(disk, sb);
    btree_insert_nonfull(disk, sb, handle, root, entry);
    btree_write_node(disk, handle, handle->root_block, root);
    btree_free_node(root);
    return;
  }

  btree_node_t *root = btree_read_node(disk, handle, handle->root_block);
  if (root->num_entries == 2 * handle->min_degree - 1) {
    btree_node_t *new_root = malloc(sizeof(btree_node_t));
    new_root->is_leaf = 0;
    new_root->num_entries = 0;
    new_root->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
    new_root->children = calloc(2 * handle->min_degree, sizeof(uint32_t));

    uint32_t new_root_block = allocate_single_block(disk, sb);
    new_root->children[0] = handle->root_block;
    btree_split_child(disk, sb, handle, new_root, 0);
    btree_insert_nonfull(disk, sb, handle, new_root, entry);
    btree_write_node(disk, handle, new_root_block, new_root);
    btree_free_node(new_root);
    handle->root_block = new_root_block;
  } else {
    btree_insert_nonfull(disk, sb, handle, root, entry);
    btree_write_node(disk, handle, handle->root_block, root);
  }
  btree_free_node(root);
}

static void btree_insert_nonfull(FILE *disk, superblock_t *sb,
                                 btree_handle_t *handle, btree_node_t *node,
                                 void *entry) {
  int i = node->num_entries - 1;
  uint8_t *entries = node->entries;
  uint32_t key = handle->get_key(entry);

  if (node->is_leaf) {
    while (i >= 0 && handle->cmp(&key, entries + i * handle->entry_size) < 0) {
      memcpy(entries + (i + 1) * handle->entry_size,
             entries + i * handle->entry_size, handle->entry_size);
      i--;
    }
    memcpy(entries + (i + 1) * handle->entry_size, entry, handle->entry_size);
    node->num_entries++;
  } else {
    while (i >= 0 && handle->cmp(&key, entries + i * handle->entry_size) < 0)
      i--;
    i++;
    btree_node_t *child = btree_read_node(disk, handle, node->children[i]);
    if (child->num_entries == 2 * handle->min_degree - 1) {
      btree_split_child(disk, sb, handle, node, i);
      if (handle->cmp(&key, entries + i * handle->entry_size) > 0)
        i++;
      btree_free_node(child);
      child = btree_read_node(disk, handle, node->children[i]);
    }
    btree_insert_nonfull(disk, sb, handle, child, entry);
    btree_write_node(disk, handle, node->children[i], child);
    btree_free_node(child);
  }
}

static void btree_split_child(FILE *disk, superblock_t *sb,
                              btree_handle_t *handle, btree_node_t *parent,
                              int index) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[index]);
  btree_node_t *new_node = malloc(sizeof(btree_node_t));
  new_node->is_leaf = child->is_leaf;
  new_node->num_entries = handle->min_degree - 1;
  new_node->entries = calloc(2 * handle->min_degree - 1, handle->entry_size);
  new_node->children = calloc(2 * handle->min_degree, sizeof(uint32_t));
  uint32_t new_block = allocate_single_block(disk, sb);

  memcpy(new_node->entries,
         (char *)child->entries + handle->min_degree * handle->entry_size,
         (handle->min_degree - 1) * handle->entry_size);

  if (!child->is_leaf) {
    memcpy(new_node->children, child->children + handle->min_degree,
           handle->min_degree * sizeof(uint32_t));
  }

  child->num_entries = handle->min_degree - 1;

  for (int j = parent->num_entries; j > index; j--) {
    parent->children[j + 1] = parent->children[j];
  }
  parent->children[index + 1] = new_block;

  for (int j = parent->num_entries - 1; j >= index; j--) {
    memcpy((char *)parent->entries + (j + 1) * handle->entry_size,
           (char *)parent->entries + j * handle->entry_size,
           handle->entry_size);
  }

  memcpy((char *)parent->entries + index * handle->entry_size,
         (char *)child->entries + (handle->min_degree - 1) * handle->entry_size,
         handle->entry_size);

  parent->num_entries++;

  btree_write_node(disk, handle, parent->children[index], child);
  btree_write_node(disk, handle, new_block, new_node);

  btree_free_node(child);
  btree_free_node(new_node);
}

// === Função de remoção completa na árvore B ===
int btree_remove(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 uint32_t key);

static void btree_remove_internal(FILE *disk, btree_handle_t *handle,
                                  uint32_t node_block, uint32_t key,
                                  int *success);
static void btree_merge(FILE *disk, btree_handle_t *handle,
                        btree_node_t *parent, int idx);
static void btree_fill(FILE *disk, btree_handle_t *handle, btree_node_t *parent,
                       int idx);
static void btree_borrow_from_prev(FILE *disk, btree_handle_t *handle,
                                   btree_node_t *parent, int idx);
static void btree_borrow_from_next(FILE *disk, btree_handle_t *handle,
                                   btree_node_t *parent, int idx);

int btree_remove(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 uint32_t key) {
  int success = 0;
  btree_remove_internal(disk, handle, node_block, key, &success);
  return success;
}

static void btree_remove_internal(FILE *disk, btree_handle_t *handle,
                                  uint32_t node_block, uint32_t key,
                                  int *success) {
  btree_node_t *node = btree_read_node(disk, handle, node_block);
  int i = 0;
  while (i < node->num_entries &&
         handle->cmp(&key, (uint8_t *)node->entries + i * handle->entry_size) >
             0)
    i++;

  if (i < node->num_entries &&
      handle->cmp(&key, (uint8_t *)node->entries + i * handle->entry_size) ==
          0) {
    if (node->is_leaf) {
      btree_shift_left(node->entries, handle->entry_size, i, node->num_entries);
      node->num_entries--;
      btree_write_node(disk, handle, node_block, node);
      *success = 1;
    } else {
      btree_node_t *pred = btree_read_node(disk, handle, node->children[i]);
      if (pred->num_entries >= handle->min_degree) {
        memcpy((uint8_t *)node->entries + i * handle->entry_size,
               (uint8_t *)pred->entries +
                   (pred->num_entries - 1) * handle->entry_size,
               handle->entry_size);
        btree_write_node(disk, handle, node_block, node);
        uint32_t pred_key =
            handle->get_key((uint8_t *)pred->entries +
                            (pred->num_entries - 1) * handle->entry_size);
        btree_free_node(pred);
        btree_remove_internal(disk, handle, node->children[i], pred_key,
                              success);
      } else {
        btree_node_t *succ =
            btree_read_node(disk, handle, node->children[i + 1]);
        if (succ->num_entries >= handle->min_degree) {
          memcpy((uint8_t *)node->entries + i * handle->entry_size,
                 (uint8_t *)succ->entries, handle->entry_size);
          btree_write_node(disk, handle, node_block, node);
          uint32_t succ_key = handle->get_key(succ->entries);
          btree_free_node(succ);
          btree_remove_internal(disk, handle, node->children[i + 1], succ_key,
                                success);
        } else {
          btree_merge(disk, handle, node, i);
          btree_remove_internal(disk, handle, node->children[i], key, success);
        }
        btree_free_node(succ);
      }
    }
  } else {
    if (node->is_leaf) {
      *success = 0;
    } else {
      int child_idx = i;
      btree_node_t *child =
          btree_read_node(disk, handle, node->children[child_idx]);
      if (child->num_entries < handle->min_degree) {
        btree_fill(disk, handle, node, child_idx);
        btree_write_node(disk, handle, node_block, node);
      }
      btree_free_node(child);
      btree_remove_internal(disk, handle, node->children[child_idx], key,
                            success);
    }
  }
  btree_free_node(node);
}

static void btree_merge(FILE *disk, btree_handle_t *handle,
                        btree_node_t *parent, int idx) {
  btree_node_t *left = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *right =
      btree_read_node(disk, handle, parent->children[idx + 1]);

  memcpy((uint8_t *)left->entries + handle->entry_size * handle->min_degree,
         (uint8_t *)right->entries, handle->entry_size * right->num_entries);

  memcpy((uint8_t *)left->entries +
             handle->entry_size * (handle->min_degree - 1),
         (uint8_t *)parent->entries + idx * handle->entry_size,
         handle->entry_size);

  if (!left->is_leaf) {
    memcpy(left->children + handle->min_degree, right->children,
           sizeof(uint32_t) * (right->num_entries + 1));
  }

  left->num_entries += right->num_entries + 1;

  for (int i = idx + 1; i < parent->num_entries; i++) {
    memcpy((uint8_t *)parent->entries + (i - 1) * handle->entry_size,
           (uint8_t *)parent->entries + i * handle->entry_size,
           handle->entry_size);
    parent->children[i] = parent->children[i + 1];
  }

  parent->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], left);
  btree_write_node(disk, handle, parent->children[idx + 1], right);
  btree_write_node(disk, handle, parent->children[idx], parent);

  btree_free_node(left);
  btree_free_node(right);
}

static void btree_fill(FILE *disk, btree_handle_t *handle, btree_node_t *parent,
                       int idx) {
  if (idx > 0) {
    btree_node_t *left =
        btree_read_node(disk, handle, parent->children[idx - 1]);
    if (left->num_entries >= handle->min_degree) {
      btree_borrow_from_prev(disk, handle, parent, idx);
      btree_free_node(left);
      return;
    }
    btree_free_node(left);
  }
  if (idx < parent->num_entries) {
    btree_node_t *right =
        btree_read_node(disk, handle, parent->children[idx + 1]);
    if (right->num_entries >= handle->min_degree) {
      btree_borrow_from_next(disk, handle, parent, idx);
      btree_free_node(right);
      return;
    }
    btree_free_node(right);
  }
  btree_merge(disk, handle, parent, (idx > 0) ? idx - 1 : idx);
}

static void btree_borrow_from_prev(FILE *disk, btree_handle_t *handle,
                                   btree_node_t *parent, int idx) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *sibling =
      btree_read_node(disk, handle, parent->children[idx - 1]);

  for (int i = child->num_entries - 1; i >= 0; i--) {
    memcpy((uint8_t *)child->entries + (i + 1) * handle->entry_size,
           (uint8_t *)child->entries + i * handle->entry_size,
           handle->entry_size);
  }

  if (!child->is_leaf) {
    for (int i = child->num_entries; i >= 0; i--) {
      child->children[i + 1] = child->children[i];
    }
  }

  memcpy(child->entries,
         (uint8_t *)parent->entries + (idx - 1) * handle->entry_size,
         handle->entry_size);

  if (!child->is_leaf) {
    child->children[0] = sibling->children[sibling->num_entries];
  }

  memcpy((uint8_t *)parent->entries + (idx - 1) * handle->entry_size,
         (uint8_t *)sibling->entries +
             (sibling->num_entries - 1) * handle->entry_size,
         handle->entry_size);

  child->num_entries++;
  sibling->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], child);
  btree_write_node(disk, handle, parent->children[idx - 1], sibling);
  btree_write_node(disk, handle, parent->children[idx], parent);

  btree_free_node(child);
  btree_free_node(sibling);
}

static void btree_borrow_from_next(FILE *disk, btree_handle_t *handle,
                                   btree_node_t *parent, int idx) {
  btree_node_t *child = btree_read_node(disk, handle, parent->children[idx]);
  btree_node_t *sibling =
      btree_read_node(disk, handle, parent->children[idx + 1]);

  memcpy((uint8_t *)child->entries + child->num_entries * handle->entry_size,
         (uint8_t *)parent->entries + idx * handle->entry_size,
         handle->entry_size);

  if (!child->is_leaf) {
    child->children[child->num_entries + 1] = sibling->children[0];
  }

  memcpy((uint8_t *)parent->entries + idx * handle->entry_size,
         sibling->entries, handle->entry_size);

  for (int i = 1; i < sibling->num_entries; i++) {
    memcpy((uint8_t *)sibling->entries + (i - 1) * handle->entry_size,
           (uint8_t *)sibling->entries + i * handle->entry_size,
           handle->entry_size);
  }

  if (!sibling->is_leaf) {
    for (int i = 0; i <= sibling->num_entries - 1; i++) {
      sibling->children[i] = sibling->children[i + 1];
    }
  }

  child->num_entries++;
  sibling->num_entries--;

  btree_write_node(disk, handle, parent->children[idx], child);
  btree_write_node(disk, handle, parent->children[idx + 1], sibling);
  btree_write_node(disk, handle, parent->children[idx], parent);

  btree_free_node(child);
  btree_free_node(sibling);
}
