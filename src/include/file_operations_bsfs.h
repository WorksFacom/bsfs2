#ifndef FILE_BSFS_H
#define FILE_BSFS_H

#include "bitmap_operations_bsfs.h"
#include "btree_bsfs.h"
#include "filesystem_bsfs.h"
#include <stdint.h>
#include <stdio.h>

// Abre um arquivo existente a partir de seu número de inode.
// Retorna ponteiro para file_t ou NULL se não encontrado.
file_t *open_file(FILE *disk, btree_handle_t *inode_tree,
                  uint32_t inode_number);

// Libera a estrutura de arquivo aberta (não persiste alterações).
void close_file(file_t *f);

// Posiciona o ponteiro em arquivo aberto
int bsfs_seek(file_t *f, uint32_t offset);

// Leitura em um arquivo aberto
int read_file(FILE *disk, btree_handle_t *bspan_tree, file_t *f,
              uint8_t *buffer, uint32_t size);

// Escrita em um arquivo aberto
int write_file(FILE *disk, btree_handle_t *bspan_tree, file_t *f,
               superblock_t *sb, const uint8_t *buffer, uint32_t size);

#endif // FILE_BSFS_H
