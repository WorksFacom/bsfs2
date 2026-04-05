/**
 * @file bspan_queue_bsfs.c
 * @brief Implementation of dynamic queue operations for BSpan allocation.
 *
 * This file provides the implementation of a FIFO queue used to manage
 * sequences of block spans (bspan_t) in the BSFS file system. These queues are
 * used during block allocation to track contiguous ranges of blocks available
 * for file storage.
 *
 * Core operations include:
 * - Initializing an empty queue
 * - Enqueuing and dequeuing bspans
 * - Copying a queue
 * - Freeing all queue elements
 * - Checking if the queue is empty
 *
 * This abstraction simplifies memory management and ordering of block span
 * assignments during file writing or preallocation.
 */

#include "../include/bspan_queue.h"

void init_bspan_queue(bspan_queue_t *queue) {
  queue->front = NULL;
  queue->rear  = NULL;
}

int enqueue_bspan(bspan_queue_t *queue, uint32_t file_blk, uint32_t disk_blk, uint16_t length) {
  bspan_node_t *new_node = (bspan_node_t *)malloc(sizeof(bspan_node_t));
  if (!new_node) {
    fprintf(stderr, "Error allocating memory for bspan on queue.\n");
    return -1;
  }
  new_node->bspan.file_blk = file_blk;
  new_node->bspan.disk_blk = disk_blk;
  new_node->bspan.length   = length;
  new_node->next           = NULL;

  if (queue->rear == NULL) {
    queue->front = new_node;
    queue->rear  = new_node;
  } else {
    queue->rear->next = new_node;
    queue->rear       = new_node;
  }
  return 0;
}

bspan_t dequeue_bspan(bspan_queue_t *queue) {
  if (queue->front == NULL) {
    fprintf(stderr, "Bspan queue is empty\n");
    exit(EXIT_FAILURE);
  }

  bspan_node_t *temp  = queue->front;
  bspan_t       bspan = temp->bspan;
  queue->front        = queue->front->next;

  if (queue->front == NULL) {
    queue->rear = NULL;
  }

  free(temp);
  return bspan;
}

bspan_queue_t *copy_bspan_queue(bspan_queue_t *original) {
  bspan_queue_t *copy = (bspan_queue_t *)malloc(sizeof(bspan_queue_t));
  if (!copy) {
    fprintf(stderr, "Error allocating memory for bspan queue copy.\n");
    return NULL;
  }

  init_bspan_queue(copy);

  bspan_node_t *current = original->front;
  while (current != NULL) {
    enqueue_bspan(copy, current->bspan.file_blk, current->bspan.disk_blk, current->bspan.length);
    current = current->next;
  }
  return copy;
}

int is_bspan_queue_empty(bspan_queue_t *queue) {
  return queue->front == NULL;
}

void free_bspan_queue(bspan_queue_t *queue) {
  while (!is_bspan_queue_empty(queue)) {
    dequeue_bspan(queue);
  }
}
