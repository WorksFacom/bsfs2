#ifndef BLOCK_OP_H
#define BLOCK_OP_H

#include "bitmap_operations_bsfs.h"
#include "filesystem_bsfs.h"

int write_block(FILE *disk, uint32_t block_number, const void *data,
                size_t size);
int read_block(FILE *disk, uint32_t block_number, void *data, size_t size);
int free_block(FILE *disk, superblock_t *sb, uint32_t block_number,
               uint8_t *bitmap, uint32_t bitmap_size);
#endif // !BLOCK_OP_H
