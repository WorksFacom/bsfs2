#ifndef BTREE_BSFS_H
#define BTREE_BSFS_H

#include "allocate_bsfs.h"
#include "block_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include <stdint.h>
#include <stdio.h>

// Tipos de dados suportados pela árvore
typedef enum {
  BTREE_TYPE_INODE,
  BTREE_TYPE_BSPAN,
  BTREE_TYPE_RECOVERY
} btree_data_type_t;

// Funções de chave e comparação
typedef uint32_t (*key_fn)(const void *entry);
typedef int (*compare_fn)(const void *a, const void *b);

// Estrutura de um nó da B-tree
typedef struct {
  uint8_t is_leaf;
  uint32_t num_entries;
  void *entries;      // Entradas genéricas
  uint32_t *children; // Ponteiros para blocos filhos
} btree_node_t;

// Manipulador de árvore
typedef struct {
  btree_data_type_t type;
  size_t entry_size;
  compare_fn cmp;
  key_fn get_key;
  uint32_t root_block;
  int min_degree;
} btree_handle_t;

// === Inicialização ===
btree_handle_t *btree_create_handle(btree_data_type_t type, size_t entry_size,
                                    compare_fn cmp, key_fn get_key);

// Manipuladores específicos
btree_handle_t *btree_create_inode_handle(void);
btree_handle_t *btree_create_bspan_handle(void);
btree_handle_t *btree_create_rec_entry_handle(void);

// === Operações principais ===
void btree_insert(FILE *disk, superblock_t *sb, btree_handle_t *handle,
                  void *entry);
int btree_search(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 uint32_t key, void *output);
int btree_update(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 void *new_entry);
int btree_remove(FILE *disk, btree_handle_t *handle, uint32_t node_block,
                 uint32_t key); // futuro

// === Operações auxiliares ===
void btree_write_node(FILE *disk, btree_handle_t *handle, uint32_t block,
                      btree_node_t *node);
btree_node_t *btree_read_node(FILE *disk, btree_handle_t *handle,
                              uint32_t block);
void btree_free_node(btree_node_t *node);

#endif // BTREE_BSFS_H
