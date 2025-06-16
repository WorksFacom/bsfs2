#include "../../include/file_operations_bsfs.h"

// === Abertura de um arquivo ===
file_t *open_file(FILE *disk, btree_handle_t *inode_tree,
                  uint32_t inode_number) {
  inode_t temp;
  if (!btree_search(disk, inode_tree, inode_tree->root_block, inode_number,
                    &temp)) {
    return NULL; // inode não encontrado
  }

  file_t *f = malloc(sizeof(file_t));
  f->inode = temp;
  f->pos = 0;
  return f;
}

// === Fechamento de um arquivo ===
void close_file(file_t *f) { free(f); }

// === Função seek que define a posição de um arquivo ===
int bsfs_seek(file_t *f, uint32_t offset) {
  if (offset > f->inode.file_size) {
    return -1; // não pode buscar além do fim do arquivo
  }
  f->pos = offset;
  return 0;
}

// === Leitura de arquivo ===
int read_file(FILE *disk, btree_handle_t *bspan_tree, file_t *f,
              uint8_t *buffer, uint32_t size) {
  uint32_t bytes_read = 0;
  uint32_t file_size = f->inode.file_size;
  uint32_t pos = f->pos;

  if (pos >= file_size)
    return 0; // fim do arquivo
  if (pos + size > file_size)
    size = file_size - pos;

  while (bytes_read < size) {
    uint32_t block_size = BLOCK_SIZE;
    uint32_t logical_block = pos / block_size;
    uint32_t block_offset = pos % block_size;
    uint32_t to_read = block_size - block_offset;
    if (to_read > size - bytes_read)
      to_read = size - bytes_read;

    bspan_t bspan_key = {.file_blk = logical_block};
    bspan_t bspan_val;
    if (!btree_search(disk, bspan_tree, bspan_tree->root_block, logical_block,
                      &bspan_val)) {
      memset(buffer + bytes_read, 0,
             to_read); // leitura de bloco não alocado = zero
    } else {
      uint8_t temp[BLOCK_SIZE];
      read_block(disk, bspan_val.disk_blk, temp, BLOCK_SIZE);
      memcpy(buffer + bytes_read, temp + block_offset, to_read);
    }

    pos += to_read;
    bytes_read += to_read;
  }

  f->pos = pos;
  return bytes_read;
}

// === Escrita de arquivo ===
int write_file(FILE *disk, btree_handle_t *bspan_tree, file_t *f,
               superblock_t *sb, const uint8_t *buffer, uint32_t size) {
  uint32_t bytes_written = 0;
  uint32_t pos = f->pos;
  uint32_t block_size = BLOCK_SIZE;
  uint32_t final_pos = pos + size;

  // Solicita blocos disponíveis para escrita com base no tamanho necessário
  uint32_t missing_bytes =
      (final_pos > f->inode.file_size) ? (final_pos - f->inode.file_size) : 0;
  uint32_t missing_blocks = (missing_bytes + block_size - 1) / block_size;
  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap)
    return -1;
  uint32_t hint_offset = 0;
  bspan_queue_t *queue;
  allocate_bspan_queue(bitmap, sb, queue, missing_blocks, hint_offset);
  if (!queue)
    return -1;

  while (bytes_written < size) {
    uint32_t logical_block = pos / block_size;
    uint32_t block_offset = pos % block_size;
    uint32_t to_write = block_size - block_offset;
    if (to_write > size - bytes_written)
      to_write = size - bytes_written;

    bspan_t bspan_val;
    if (!btree_search(disk, bspan_tree, bspan_tree->root_block, logical_block,
                      &bspan_val)) {
      if (is_bspan_queue_empty(queue))
        break;
      bspan_t new_bspan = dequeue_bspan(queue);
      new_bspan.file_blk = logical_block;
      btree_insert(disk, sb, bspan_tree, &new_bspan);
      bspan_val = new_bspan;
    }

    uint8_t temp[BLOCK_SIZE] = {0};
    if (block_offset != 0 || to_write < BLOCK_SIZE) {
      read_block(disk, bspan_val.disk_blk, temp, BLOCK_SIZE);
    }

    memcpy(temp + block_offset, buffer + bytes_written, to_write);
    write_block(disk, bspan_val.disk_blk, temp, BLOCK_SIZE);

    pos += to_write;
    bytes_written += to_write;
  }

  if (pos > f->inode.file_size) {
    f->inode.file_size = pos;
  }
  f->pos = pos;

  free_bspan_queue(queue);
  free(bitmap);
  return bytes_written;
}
