#ifndef BSPAN_QUEUE_H
#define BSPAN_QUEUE_H

#include "filesystem_bsfs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct bspan_node {
  bspan_t bspan;
  struct bspan_node *next;
} bspan_node_t;

typedef struct {
  bspan_node_t *front;
  bspan_node_t *rear;
} bspan_queue_t;

void init_bspan_queue(bspan_queue_t *queue);
int enqueue_bspan(bspan_queue_t *queue, uint32_t file_blk, uint32_t disk_blk,
                  uint16_t length);
bspan_t dequeue_bspan(bspan_queue_t *queue);
bspan_queue_t *copy_bspan_queue(bspan_queue_t *original);
int is_bspan_queue_empty(bspan_queue_t *queue);
void free_bspan_queue(bspan_queue_t *queue);

#endif // !BSPAN_QUEUE_H
