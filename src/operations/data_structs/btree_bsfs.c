/*
 * Arquivo btree_bsfs.c com a árvore de bspan, inodes e rec_entry
 *
 * Operações que a árvore B precisa realizar:
 *
 * Inicialização:
 * 	btree_init_node: inicializa a estrutura de um nó vazio
 * 	btree_create_root: criação da raiz da árvore
 *
 * Leitura e escrita
 * 	serialize_node: serializa nó para escrita
 * 	deserialize_node: desserializa nó para leitura
 * 	btree_read_node: lê um nó do disco
 *  btree_write_node: escreve um nó da memória de volta no disco
 *
 * Inserção de nós:
 *  is_node_full: verifica se o nó está cheio
 *  btree_split_child: divide um filho cheio do parent, promovendo o do meio
 *  btree_insert_nonfull: insere entrada em nó não cheio
 *  para o pai
 *  btree_insert: insere uma entrada na árvore começando pela raiz
 *
 * Busca:
 *  btree_search: procura a entrada correta na árvore baseada no índice
 *  btree_update: atualiza uma entrada existente ou insere uma nova
 *
 * Remoção:
 * 	btree_find_index
 * 	btree_get_predecessor
 * 	btree_get_sucessor
 * 	btree_borrow_from_prev
 * 	btree_borrow_from_next
 * 	btree_delete_recursive
 * 	btree_delete
 * */

#include "../../include/btree_bsfs.h"

// Função de comparação de bspans necessária para outras funções
static int compare_bspan(const bspan_t *a, const bspan_t *b) {
  if (a->file_blk < b->file_blk)
    return -1;
  if (a->file_blk > b->file_blk)
    return 1;
  return 0;
}

// Função de comparação de inodes necessária para outras funções
static int compare_inode(const inode_t *a, const inode_t *b) {
  if (a->inode_number < b->inode_number)
    return -1;
  if (a->inode_number > b->inode_number)
    return 1;
  return 0;
}

// Função de comparação de entradas de recuperação necessária para outras
// funções
static int compare_rec_entry(const rec_entry_t *a, const rec_entry_t *b) {
  if (a->index < b->index)
    return -1;
  if (a->index > b->index)
    return 1;
  return 0;
}

// Função que retorna um comparador a partir do data type da btree
int (*get_comparator(btree_data_type_t type))(const void *, const void *) {
  switch (type) {
  case BSPAN:
    return (int (*)(const void *, const void *))compare_bspan;
  case INODE:
    return (int (*)(const void *, const void *))compare_inode;
  case REC_ENTRY:
    return (int (*)(const void *, const void *))compare_rec_entry;
  default:
    return NULL;
  }
}

// Função para obter o tamanho de um dado
size_t get_entry_size(btree_data_type_t type) {
  switch (type) {
  case BSPAN:
    return sizeof(bspan_t);
  case INODE:
    return sizeof(inode_t);
  case REC_ENTRY:
    return sizeof(rec_entry_t);
  default:
    return 0;
  }
}

// Retorna o número máximo de chaves por tipo
size_t btree_max_keys(btree_data_type_t type) {
  switch (type) {
  case BSPAN:
    return (BLOCK_SIZE - (2 * sizeof(uint32_t)) - sizeof(uint8_t)) /
           (sizeof(bspan_t) + sizeof(uint32_t));
  case INODE:
    return (BLOCK_SIZE - (2 * sizeof(uint32_t)) - sizeof(uint8_t)) /
           (sizeof(inode_t) + sizeof(uint32_t));
  case REC_ENTRY:
    return (BLOCK_SIZE - (2 * sizeof(uint32_t)) - sizeof(uint8_t)) /
           (sizeof(rec_entry_t) + sizeof(uint32_t));
  default:
    return 0;
  }
}

// Função que verifica se um nó da Btree está cheio
static int is_node_full(btree_node_t *node, btree_data_type_t data_type) {
  return (node->num_entries >= btree_max_keys(data_type));
}

// Função para serializar um nó
void serialize_node(btree_node_t *node, uint8_t *buffer, btree_data_type_t type,
                    size_t key_size) {
  size_t offset = 0;

  buffer[offset++] = node->is_leaf;
  memcpy(buffer + offset, &node->num_entries, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  memcpy(buffer + offset, &node->parent, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  for (uint32_t i = 0; i < node->num_entries; i++) {
    memcpy(buffer + offset, (uint8_t *)node->entries + i * key_size, key_size);
    offset += key_size;
  }

  if (!node->is_leaf) {
    for (uint32_t i = 0; i <= node->num_entries; i++) {
      memcpy(buffer + offset, &node->children[i], sizeof(uint32_t));
      offset += sizeof(uint32_t);
    }
  }
}

// Função para desserializar um nó
void deserialize_node(uint8_t *buffer, btree_node_t *node,
                      btree_data_type_t type, size_t key_size) {
  size_t offset = 0;

  node->is_leaf = buffer[offset++];
  memcpy(&node->num_entries, buffer + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  memcpy(&node->parent, buffer + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  size_t max_keys = btree_max_keys(type);
  node->entries = malloc(key_size * max_keys);
  if (!node->entries) {
    fprintf(stderr, "Error allocating memory for keys\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < node->num_entries; i++) {
    memcpy((uint8_t *)node->entries + i * key_size, buffer + offset, key_size);
    offset += key_size;
  }

  if (!node->is_leaf) {
    node->children = malloc(sizeof(uint32_t) * (max_keys + 1));
    if (!node->children) {
      fprintf(stderr, "Error allocating memory for children\n");
      exit(EXIT_FAILURE);
    }
    for (uint32_t i = 0; i <= node->num_entries; i++) {
      memcpy(&node->children[i], buffer + offset, sizeof(uint32_t));
      offset += sizeof(uint32_t);
    }
  } else {
    node->children = NULL;
  }
}

// Escrita de um nó
int btree_write_node(FILE *disk, uint32_t block_number, btree_node_t *node,
                     btree_data_type_t type) {
  uint8_t buffer[BLOCK_SIZE];
  serialize_node(node, buffer, type, get_entry_size(type));
  return write_block(disk, block_number, buffer, BLOCK_SIZE);
}

// Leitura de um nó
int btree_read_node(FILE *disk, uint32_t block_number, btree_node_t *node,
                    btree_data_type_t type) {
  uint8_t buffer[BLOCK_SIZE];
  if (read_block(disk, block_number, buffer, BLOCK_SIZE) != 0)
    return -1;
  deserialize_node(buffer, node, type, get_entry_size(type));
  return 0;
}

/**
 * @brief Initializes a B-tree node in memory.
 *
 * @param node Pointer to the node to initialize.
 * @param is_leaf 1 if the node is a leaf, 0 otherwise.
 * @param data_type The type of data the tree stores (BSPAN, INODE, REC_ENTRY).
 *
 * @return 0 on success, -1 on memory allocation failure.
 */
int btree_init_node(btree_node_t *node, uint8_t is_leaf,
                    btree_data_type_t data_type) {
  node->is_leaf = is_leaf;
  node->num_entries = 0;
  node->parent = 0; // Pode ser 0 ou outro valor conforme seu sistema

  size_t key_size = get_entry_size(data_type);
  uint32_t max_keys = btree_max_keys(data_type); // usa o valor do enum

  // Aloca memória para as chaves
  node->entries = malloc(key_size * max_keys);
  if (!node->entries)
    return -1;

  // Aloca memória para os filhos se não for folha
  if (!is_leaf) {
    node->children = malloc(sizeof(uint32_t) * (max_keys + 1));
    if (!node->children) {
      free(node->entries);
      return -1;
    }
  } else {
    node->children = NULL;
  }

  return 0;
}

void btree_free_node(btree_node_t *node) {
  if (node->entries)
    free(node->entries);
  if (node->children)
    free(node->children);
}

/**
 * @brief Libera recursivamente todos os nós da árvore B, marcando seus blocos
 * como livres.
 *
 * @param disk Ponteiro para o arquivo do disco.
 * @param node_block Número do bloco do nó atual.
 * @param bitmap Bitmap para marcar blocos como livres.
 * @param type Tipo de dados da árvore.
 * @return 0 em caso de sucesso, -1 em caso de falha.
 */
int btree_free_recursive(FILE *disk, uint32_t node_block, uint8_t *bitmap,
                         btree_data_type_t type) {
  // Verifica parâmetros
  if (!disk || !bitmap || node_block == INVALID_BLOCK) {
    return -1;
  }

  // Lê o nó do disco
  btree_node_t node;
  if (btree_read_node(disk, node_block, &node, type) != 0) {
    fprintf(stderr, "Failed to read node at block %u during recursive free.\n",
            node_block);
    return -1;
  }

  // Se não é folha, libera recursivamente todos os filhos primeiro
  if (!node.is_leaf) {
    for (uint32_t i = 0; i <= node.num_entries; i++) {
      if (node.children[i] != INVALID_BLOCK) {
        if (btree_free_recursive(disk, node.children[i], bitmap, type) != 0) {
          btree_free_node(&node);
          return -1;
        }
      }
    }
  }

  // Libera as estruturas de memória do nó
  btree_free_node(&node);

  // Marca o bloco como livre no bitmap
  clear_bit(bitmap, node_block);

  return 0;
}

/**
 * @brief Creates the root node of a B-tree and writes it to disk.
 *
 * @param disk Pointer to the opened disk file.
 * @param data_type The type of data this B-tree will store.
 * @param bitmap Bitmap indicating free/used blocks.
 * @param bitmap_start_block First block of the bitmap on disk.
 * @param bitmap_total_blocks Number of blocks used by the bitmap.
 * @param[out] root_block Output parameter: the block number where the root was
 * created.
 *
 * @return 0 on success, -1 on failure.
 */
uint32_t btree_create_root(FILE *disk, superblock_t *sb,
                           btree_data_type_t data_type) {
  uint32_t root_block;
  btree_node_t root;
  if (btree_init_node(&root, 1 /* is_leaf */, data_type) != 0) {
    fprintf(stderr, "Failed to initialize B-tree root node.\n");
    return -1;
  }

  uint8_t *bitmap = load_bitmap(disk, sb->total_blocks);

  // Aloca um bloco livre para a raiz
  int32_t block = allocate_single_block(disk, sb);
  if (block < 0) {
    fprintf(stderr, "No free block available for B-tree root.\n");
    btree_free_node(&root);
    return INVALID_BLOCK;
  }

  set_bit(bitmap, block); // Marca o bloco como usado

  // Salva bitmap atualizado no disco
  if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
    btree_free_node(&root);
    return INVALID_BLOCK;
  }

  // Escreve o nó raiz no disco
  if (btree_write_node(disk, block, &root, data_type) != 0) {
    btree_free_node(&root);
    return -1;
  }

  root_block = (uint32_t)block;
  free(bitmap);
  btree_free_node(&root);
  return root_block;
}

// Função que divide um nó nos filhos
int btree_split_child(FILE *disk, superblock_t *sb, uint32_t parent_blk,
                      uint32_t child_index, uint32_t full_child_blk,
                      btree_data_type_t type) {
  size_t key_size = get_entry_size(type);
  uint32_t max_keys = type;

  btree_node_t parent, full_child, new_child;
  uint8_t *bitmap = load_bitmap(disk, sb->total_blocks);

  // Lê o nó pai e o filho cheio
  if (btree_read_node(disk, parent_blk, &parent, type) != 0 ||
      btree_read_node(disk, full_child_blk, &full_child, type) != 0) {
    free(bitmap);
    return -1;
  }

  // Inicializa novo nó
  if (btree_init_node(&new_child, full_child.is_leaf, type) != 0) {
    fprintf(stderr, "Failed to initialize new child during split.\n");
    btree_free_node(&parent);
    btree_free_node(&full_child);
    free(bitmap);
    return -1;
  }

  // Aloca bloco para o novo nó
  int32_t new_child_blk = allocate_single_block(disk, sb);
  if (new_child_blk < 0) {
    fprintf(stderr, "No free block for splitting child.\n");
    btree_free_node(&parent);
    btree_free_node(&full_child);
    btree_free_node(&new_child);
    free(bitmap);
    return -1;
  }

  set_bit(bitmap, new_child_blk);

  // Número de chaves a mover (metade)
  uint32_t mid = max_keys / 2;

  // Copia segunda metade das chaves do filho cheio para o novo filho
  for (uint32_t i = 0; i < mid; i++) {
    memcpy((uint8_t *)new_child.entries + i * key_size,
           (uint8_t *)full_child.entries + (mid + 1 + i) * key_size, key_size);
  }

  // Copia filhos se não for folha
  if (!full_child.is_leaf) {
    for (uint32_t i = 0; i <= mid; i++) {
      new_child.children[i] = full_child.children[mid + 1 + i];
    }
  }

  new_child.num_entries = mid;
  full_child.num_entries = mid;

  // Move filhos do pai para abrir espaço para o novo
  for (uint32_t i = parent.num_entries; i > child_index; i--) {
    parent.children[i + 1] = parent.children[i];
  }
  parent.children[child_index + 1] = new_child_blk;

  // Move chaves do pai para abrir espaço para a promovida
  for (uint32_t i = parent.num_entries; i > child_index; i--) {
    memcpy((uint8_t *)parent.entries + i * key_size,
           (uint8_t *)parent.entries + (i - 1) * key_size, key_size);
  }

  // Promove chave do meio
  memcpy((uint8_t *)parent.entries + child_index * key_size,
         (uint8_t *)full_child.entries + mid * key_size, key_size);
  parent.num_entries++;

  // Salva tudo no disco
  write_block(disk, sb->block_bitmap_start, bitmap, BLOCK_SIZE);
  btree_write_node(disk, parent_blk, &parent, type);
  btree_write_node(disk, full_child_blk, &full_child, type);
  btree_write_node(disk, new_child_blk, &new_child, type);

  btree_free_node(&parent);
  btree_free_node(&full_child);
  btree_free_node(&new_child);
  free(bitmap);

  return 0;
}

// Função que insere um valor em um nó não cheio
int btree_insert_nonfull(FILE *disk, superblock_t *sb, btree_node_t *node,
                         uint32_t node_block, void *entry,
                         btree_data_type_t type) {
  size_t key_size = get_entry_size(type);
  int (*cmp)(const void *, const void *) = get_comparator(type);

  int i = node->num_entries - 1;

  if (node->is_leaf) {
    // Move as chaves maiores para abrir espaço para a nova entrada
    while (i >= 0 && cmp(entry, (uint8_t *)node->entries + i * key_size) < 0) {
      memcpy((uint8_t *)node->entries + (i + 1) * key_size,
             (uint8_t *)node->entries + i * key_size, key_size);
      i--;
    }

    // Insere a nova chave
    memcpy((uint8_t *)node->entries + (i + 1) * key_size, entry, key_size);
    node->num_entries++;

    // Escreve o nó atualizado no disco
    if (btree_write_node(disk, node_block, node, type) != 0) {
      fprintf(stderr, "Failed to write updated leaf node.\n");
      return -1;
    }
  } else {
    // Encontrar o filho apropriado
    while (i >= 0 && cmp(entry, (uint8_t *)node->entries + i * key_size) < 0) {
      i--;
    }
    i++; // Filho correto

    // Lê o filho do disco
    btree_node_t child;
    if (btree_read_node(disk, node->children[i], &child, type) != 0) {
      fprintf(stderr, "Failed to read child node at index %d.\n", i);
      return -1;
    }

    // Se o filho está cheio, dividimos ele
    if (child.num_entries >= type) {
      if (btree_split_child(disk, sb, node_block, i, node->children[i], type) !=
          0) {
        return -1;
      }

      // Releia a chave promovida para comparar com a entrada
      if (cmp(entry, (uint8_t *)node->entries + i * key_size) > 0) {
        i++; // Avança para o novo filho à direita
      }

      // Releitura do novo filho, já que split pode ter movido os ponteiros
      btree_free_node(&child);
      if (btree_read_node(disk, node->children[i], &child, type) != 0) {
        fprintf(stderr, "Failed to read new child after split.\n");
        return -1;
      }
    }

    // Chamada recursiva para inserir no filho apropriado
    int res =
        btree_insert_nonfull(disk, sb, &child, node->children[i], entry, type);
    btree_free_node(&child);
    return res;
  }

  return 0;
}

// Função de inserção de um valor na árvore
int btree_insert(FILE *disk, superblock_t *sb, uint32_t *root_block, void *key,
                 btree_data_type_t type) {
  size_t key_size = get_entry_size(type);
  int (*cmp)(const void *, const void *) = get_comparator(type);

  btree_node_t root;
  if (btree_read_node(disk, *root_block, &root, type) != 0) {
    fprintf(stderr, "Failed to read root node.\n");
    return -1;
  }

  if (is_node_full(&root, type)) {
    // Cria novo nó raiz
    btree_node_t new_root;
    if (btree_init_node(&new_root, 0 /* is_leaf = false */, type) != 0) {
      fprintf(stderr, "Failed to initialize new root node.\n");
      btree_free_node(&root);
      return -1;
    }

    uint8_t *bitmap = load_bitmap(disk, sb->total_blocks);
    int32_t new_root_block = allocate_single_block(disk, sb);
    if (new_root_block < 0) {
      fprintf(stderr, "No free block for new root.\n");
      free(bitmap);
      btree_free_node(&root);
      btree_free_node(&new_root);
      return -1;
    }

    set_bit(bitmap, new_root_block);
    new_root.children[0] = *root_block;

    // Salva bitmap atualizado
    if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
      free(bitmap);
      btree_free_node(&root);
      btree_free_node(&new_root);
      return -1;
    }
    free(bitmap);

    // Divide a raiz antiga
    if (btree_split_child(disk, sb, new_root_block, 0, *root_block, type) !=
        0) {
      btree_free_node(&root);
      btree_free_node(&new_root);
      return -1;
    }

    // Insere no novo nó
    if (btree_insert_nonfull(disk, sb, &root, *root_block, key, type) != 0) {
      btree_free_node(&root);
      btree_free_node(&new_root);
      return -1;
    }

    // Atualiza a nova raiz no disco
    if (btree_write_node(disk, new_root_block, &new_root, type) != 0) {
      btree_free_node(&root);
      btree_free_node(&new_root);
      return -1;
    }

    *root_block = new_root_block;
    btree_free_node(&root);
    btree_free_node(&new_root);
  } else {
    // Raiz não está cheia, insere diretamente
    int result = btree_insert_nonfull(disk, sb, &root, *root_block, key, type);
    if (result != 0) {
      btree_free_node(&root);
      return result;
    }

    if (btree_write_node(disk, *root_block, &root, type) != 0) {
      btree_free_node(&root);
      return -1;
    }

    btree_free_node(&root);
  }

  return 0;
}

// Função de busca na árvore
int btree_search(FILE *disk, uint32_t root_block, uint32_t index, void *output,
                 btree_data_type_t type) {
  size_t index_size = sizeof(uint32_t);
  size_t key_size = get_entry_size(type);
  int (*cmp)(const void *, const void *) = get_comparator(type);

  btree_node_t node;
  if (btree_read_node(disk, root_block, &node, type) != 0) {
    fprintf(stderr, "Failed to read node at block %u\n", root_block);
    return 0;
  }

  int i = 0;
  while (i < node.num_entries &&
         cmp(&index, (uint8_t *)node.entries + i * index_size) > 0) {
    i++;
  }

  if (i < node.num_entries &&
      cmp(&index, (uint8_t *)node.entries + i * index_size) == 0) {
    // Chave encontrada, copia a entrada correspondente
    memcpy(output, (uint8_t *)node.entries + i * key_size, key_size);
    btree_free_node(&node);
    return 1;
  }

  if (node.is_leaf) {
    btree_free_node(&node);
    return 0; // Não encontrado
  }

  // Chave ainda não encontrada, desce para o filho apropriado
  uint32_t child_block = node.children[i];
  btree_free_node(&node);
  return btree_search(disk, child_block, index, output, type);
}

// Atualização de uma entrada da árvore
int btree_update_entry(FILE *disk, uint32_t root_block, uint32_t index,
                       void *new_entry, btree_data_type_t type) {
  size_t index_size = sizeof(uint32_t);
  size_t entry_size = get_entry_size(type);
  int (*cmp)(const void *, const void *) = get_comparator(type);

  btree_node_t node;
  if (btree_read_node(disk, root_block, &node, type) != 0) {
    fprintf(stderr, "Failed to read node at block %u\n", root_block);
    return -1;
  }

  int i = 0;
  while (i < node.num_entries &&
         cmp(&index, (uint8_t *)node.entries + i * index_size) > 0) {
    i++;
  }

  if (i < node.num_entries &&
      cmp(&index, (uint8_t *)node.entries + i * index_size) == 0) {
    // Entrada encontrada — atualiza
    memcpy((uint8_t *)node.entries + i * entry_size, new_entry, entry_size);
    if (btree_write_node(disk, root_block, &node, type) != 0) {
      fprintf(stderr, "Failed to write updated node at block %u\n", root_block);
      btree_free_node(&node);
      return -1;
    }
    btree_free_node(&node);
    return 0;
  }

  if (node.is_leaf) {
    btree_free_node(&node);
    return -1; // Não encontrado
  }

  uint32_t child_block = node.children[i];
  btree_free_node(&node);
  return btree_update_entry(disk, child_block, index, new_entry, type);
}

// Obtém o índice de uma entrada no nó
int btree_find_entry_index(btree_node_t *node, uint32_t key,
                           btree_data_type_t type, int *found) {
  size_t key_size = sizeof(uint32_t);
  int (*cmp)(const void *, const void *) = get_comparator(type);

  int i = 0;
  while (i < node->num_entries &&
         cmp(&key, (uint8_t *)node->entries + i * key_size) > 0) {
    i++;
  }

  if (i < node->num_entries &&
      cmp(&key, (uint8_t *)node->entries + i * key_size) == 0) {
    *found = 1;
    return i;
  }

  *found = 0;
  return i; // Posição onde a chave deveria estar
}

// Função para obter o predecessor
int btree_get_predecessor(FILE *disk, uint32_t node_block, void *output,
                          btree_data_type_t type) {
  btree_node_t node;
  if (btree_read_node(disk, node_block, &node, type) != 0) {
    fprintf(stderr, "Failed to read node for predecessor.\n");
    return -1;
  }

  while (!node.is_leaf) {
    uint32_t child_block = node.children[node.num_entries]; // Último filho
    btree_free_node(&node);
    if (btree_read_node(disk, child_block, &node, type) != 0) {
      fprintf(stderr, "Failed to read child during predecessor search.\n");
      return -1;
    }
  }

  size_t entry_size = get_entry_size(type);
  memcpy(output, (uint8_t *)node.entries + (node.num_entries - 1) * entry_size,
         entry_size);
  btree_free_node(&node);
  return 0;
}

// Função para obter o sucessor
int btree_get_successor(FILE *disk, uint32_t node_block, void *output,
                        btree_data_type_t type) {
  btree_node_t node;
  if (btree_read_node(disk, node_block, &node, type) != 0) {
    fprintf(stderr, "Failed to read node for successor.\n");
    return -1;
  }

  while (!node.is_leaf) {
    uint32_t child_block = node.children[0]; // Primeiro filho
    btree_free_node(&node);
    if (btree_read_node(disk, child_block, &node, type) != 0) {
      fprintf(stderr, "Failed to read child during successor search.\n");
      return -1;
    }
  }

  size_t entry_size = get_entry_size(type);
  memcpy(output, (uint8_t *)node.entries, entry_size); // Primeira entrada
  btree_free_node(&node);
  return 0;
}

// Função para unir dois nós
int btree_merge_children(FILE *disk, superblock_t *sb, uint32_t parent_blk,
                         int index, btree_data_type_t type) {
  size_t entry_size = get_entry_size(type);
  uint8_t *bitmap = load_bitmap(disk, sb->total_blocks);
  if (!bitmap)
    return -1;

  btree_node_t parent, left, right;

  if (btree_read_node(disk, parent_blk, &parent, type) != 0) {
    free(bitmap);
    return -1;
  }

  uint32_t left_blk = parent.children[index];
  uint32_t right_blk = parent.children[index + 1];

  if (btree_read_node(disk, left_blk, &left, type) != 0 ||
      btree_read_node(disk, right_blk, &right, type) != 0) {
    btree_free_node(&parent);
    free(bitmap);
    return -1;
  }

  // Insere a chave do meio do pai no final do filho esquerdo
  memcpy((uint8_t *)left.entries + left.num_entries * entry_size,
         (uint8_t *)parent.entries + index * entry_size, entry_size);
  left.num_entries++;

  // Copia entradas do filho direito
  for (int i = 0; i < right.num_entries; i++) {
    memcpy((uint8_t *)left.entries + (left.num_entries + i) * entry_size,
           (uint8_t *)right.entries + i * entry_size, entry_size);
  }

  // Copia filhos, se não for folha
  if (!left.is_leaf) {
    for (int i = 0; i <= right.num_entries; i++) {
      left.children[left.num_entries + i] = right.children[i];
    }
  }

  left.num_entries += right.num_entries;

  // Remove chave e filho direito do pai
  for (int i = index; i < parent.num_entries - 1; i++) {
    memcpy((uint8_t *)parent.entries + i * entry_size,
           (uint8_t *)parent.entries + (i + 1) * entry_size, entry_size);
    parent.children[i + 1] = parent.children[i + 2];
  }
  parent.num_entries--;

  // Libera o bloco do nó direito
  clear_bit(bitmap, right_blk);

  // Escreve alterações
  write_block(disk, sb->block_bitmap_start, bitmap, BLOCK_SIZE);
  btree_write_node(disk, left_blk, &left, type);
  btree_write_node(disk, parent_blk, &parent, type);

  btree_free_node(&parent);
  btree_free_node(&left);
  btree_free_node(&right);
  free(bitmap);

  return 0;
}

int btree_borrow_from_prev(FILE *disk, superblock_t *sb, uint32_t parent_blk,
                           int index, btree_data_type_t type) {
  size_t entry_size = get_entry_size(type);

  btree_node_t parent, child, sibling;

  if (btree_read_node(disk, parent_blk, &parent, type) != 0)
    return -1;

  if (btree_read_node(disk, parent.children[index], &child, type) != 0) {
    btree_free_node(&parent);
    return -1;
  }

  if (btree_read_node(disk, parent.children[index - 1], &sibling, type) != 0) {
    btree_free_node(&parent);
    btree_free_node(&child);
    return -1;
  }

  // Move entradas no filho para frente
  for (int i = child.num_entries; i > 0; i--) {
    memcpy((uint8_t *)child.entries + i * entry_size,
           (uint8_t *)child.entries + (i - 1) * entry_size, entry_size);
  }

  // Se não é folha, move ponteiros de filhos
  if (!child.is_leaf) {
    for (int i = child.num_entries + 1; i > 0; i--) {
      child.children[i] = child.children[i - 1];
    }
  }

  // Move chave do pai para início do filho
  memcpy((uint8_t *)child.entries,
         (uint8_t *)parent.entries + (index - 1) * entry_size, entry_size);

  // Move última entrada do irmão para pai
  memcpy((uint8_t *)parent.entries + (index - 1) * entry_size,
         (uint8_t *)sibling.entries + (sibling.num_entries - 1) * entry_size,
         entry_size);

  if (!child.is_leaf) {
    child.children[0] = sibling.children[sibling.num_entries];
  }

  child.num_entries++;
  sibling.num_entries--;

  btree_write_node(disk, parent_blk, &parent, type);
  btree_write_node(disk, parent.children[index], &child, type);
  btree_write_node(disk, parent.children[index - 1], &sibling, type);

  btree_free_node(&parent);
  btree_free_node(&child);
  btree_free_node(&sibling);

  return 0;
}

// Função para mover entrada do nó seguinte
int btree_borrow_from_next(FILE *disk, superblock_t *sb, uint32_t parent_blk,
                           int index, btree_data_type_t type) {
  size_t entry_size = get_entry_size(type);

  btree_node_t parent, child, sibling;

  if (btree_read_node(disk, parent_blk, &parent, type) != 0)
    return -1;

  if (btree_read_node(disk, parent.children[index], &child, type) != 0) {
    btree_free_node(&parent);
    return -1;
  }

  if (btree_read_node(disk, parent.children[index + 1], &sibling, type) != 0) {
    btree_free_node(&parent);
    btree_free_node(&child);
    return -1;
  }

  // Move chave do pai para o final do filho
  memcpy((uint8_t *)child.entries + child.num_entries * entry_size,
         (uint8_t *)parent.entries + index * entry_size, entry_size);

  // Se não é folha, mover ponteiro de filho
  if (!child.is_leaf) {
    child.children[child.num_entries + 1] = sibling.children[0];
  }

  // Move primeira entrada do irmão para o pai
  memcpy((uint8_t *)parent.entries + index * entry_size,
         (uint8_t *)sibling.entries, entry_size);

  // Desloca entradas e filhos do irmão para esquerda
  for (int i = 1; i < sibling.num_entries; i++) {
    memcpy((uint8_t *)sibling.entries + (i - 1) * entry_size,
           (uint8_t *)sibling.entries + i * entry_size, entry_size);
  }

  if (!sibling.is_leaf) {
    for (int i = 1; i <= sibling.num_entries; i++) {
      sibling.children[i - 1] = sibling.children[i];
    }
  }

  child.num_entries++;
  sibling.num_entries--;

  btree_write_node(disk, parent_blk, &parent, type);
  btree_write_node(disk, parent.children[index], &child, type);
  btree_write_node(disk, parent.children[index + 1], &sibling, type);

  btree_free_node(&parent);
  btree_free_node(&child);
  btree_free_node(&sibling);

  return 0;
}

int btree_ensure_child_has_enough_entries(FILE *disk, superblock_t *sb,
                                          uint32_t parent_blk, int index,
                                          btree_data_type_t type) {
  int result = 0;
  btree_node_t parent, child;

  if (btree_read_node(disk, parent_blk, &parent, type) != 0)
    return -1;

  if (btree_read_node(disk, parent.children[index], &child, type) != 0) {
    btree_free_node(&parent);
    return -1;
  }

  uint32_t min_entries = btree_max_keys(type) / 2;

  if (child.num_entries >= min_entries) {
    btree_free_node(&parent);
    btree_free_node(&child);
    return 0; // Já tem entradas suficientes
  }

  // Tenta emprestar do irmão à esquerda
  if (index > 0) {
    btree_node_t left_sibling;
    if (btree_read_node(disk, parent.children[index - 1], &left_sibling,
                        type) == 0) {
      if (left_sibling.num_entries > min_entries) {
        result = btree_borrow_from_prev(disk, sb, parent_blk, index, type);
        btree_free_node(&left_sibling);
        btree_free_node(&parent);
        btree_free_node(&child);
        return result;
      }
      btree_free_node(&left_sibling);
    }
  }

  // Tenta emprestar do irmão à direita
  if (index < parent.num_entries) {
    btree_node_t right_sibling;
    if (btree_read_node(disk, parent.children[index + 1], &right_sibling,
                        type) == 0) {
      if (right_sibling.num_entries > min_entries) {
        result = btree_borrow_from_next(disk, sb, parent_blk, index, type);
        btree_free_node(&right_sibling);
        btree_free_node(&parent);
        btree_free_node(&child);
        return result;
      }
      btree_free_node(&right_sibling);
    }
  }

  // Nenhum irmão pôde emprestar, faz merge
  if (index < parent.num_entries) {
    result = btree_merge_children(disk, sb, parent_blk, index, type);
  } else {
    result = btree_merge_children(disk, sb, parent_blk, index - 1, type);
  }

  btree_free_node(&parent);
  btree_free_node(&child);
  return result;
}
