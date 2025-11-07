/**
 * @file bspan_queue_bsfs.h
 * @brief Header file for managing queues of BSpan entries in the BSFS file
 * system.
 *
 * This file defines the data structures and types used for managing a FIFO
 * queue of block spans (bspan_t), which represent contiguous sequences of
 * allocated blocks. These queues are used internally during allocation
 * operations (e.g., when allocating space for a file) to track which disk
 * blocks have been reserved and in what order.
 */
#ifndef BSPAN_QUEUE_H
#define BSPAN_QUEUE_H

#include "filesystem_bsfs.h"

/**
 * @struct bspan_node
 * @brief Node structure for a BSpan queue.
 *
 * Represents a single element in a FIFO queue of `bspan_t` entries,
 * each describing a span of contiguous blocks on disk.
 */
typedef struct bspan_node {
  /**
   * The BSpan element contained in this node, describing a range of
   * logical-to-physical block mappings.
   */
  bspan_t bspan;

  /**
   * Pointer to the next node in the queue. If NULL, this is the last node.
   */
  struct bspan_node *next;
} bspan_node_t; /**< Node element used inside a BSpan FIFO queue. */

/**
 * @struct bspan_queue_t
 * @brief FIFO queue structure for managing BSpan entries.
 *
 * Used to track multiple `bspan_t` allocations in sequence, typically during
 * block allocation for file writes. Supports dynamic, ordered allocation.
 */
typedef struct {
  /**
   * Pointer to the front (head) node of the queue.
   */
  bspan_node_t *front;

  /**
   * Pointer to the rear (tail) node of the queue.
   */
  bspan_node_t *rear;
} bspan_queue_t;

/**
 * @brief Initializes a bspan queue.
 *
 * The function sets the `front` and `rear` pointers of the queue to `NULL`,
 * indicating that the queue is empty.
 *
 * @param queue Pointer to the bspan queue to be initialized.
 */
void init_bspan_queue(bspan_queue_t *queue);

/**
 * @brief Enqueues a bspan into the queue.
 *
 * This function creates a new node to store the bspan and adds it to the
 * end of the queue. If the queue is empty, the new node becomes both the
 * front and the rear of the queue.
 *
 * @param queue Pointer to the bspan queue where the bspan will be added.
 * @param file_blk The file block associated with the bspan.
 * @param disk_blk The disk block associated with the bspan.
 * @param length The length of the bspan.
 * @return 0 on success, -1 if memory allocation fails.
 */
int enqueue_bspan(bspan_queue_t *queue, uint32_t file_blk, uint32_t disk_blk,
                  uint16_t length);

/**
 * @brief Dequeues and returns the bspan from the front of the queue.
 *
 * This function removes the first bspan from the queue and returns it. If
 * the queue is empty, the program exits with an error.
 *
 * @param queue Pointer to the bspan queue from which the bspan will be
 * dequeued.
 * @return The bspan removed from the front of the queue.
 */
bspan_t dequeue_bspan(bspan_queue_t *queue);

/**
 * @brief Copies a bspan queue.
 *
 * This function creates a new queue and copies all the elements from the
 * original queue into the new queue. The new queue is returned.
 *
 * @param original Pointer to the bspan queue to be copied.
 * @return Pointer to the new copied bspan queue.
 */
bspan_queue_t *copy_bspan_queue(bspan_queue_t *original);

/**
 * @brief Checks if the bspan queue is empty.
 *
 * This function returns 1 if the queue is empty, and 0 otherwise.
 *
 * @param queue Pointer to the bspan queue to be checked.
 * @return 1 if the queue is empty, 0 otherwise.
 */
int is_bspan_queue_empty(bspan_queue_t *queue);

/**
 * @brief Frees all elements of the bspan queue.
 *
 * This function removes all elements from the queue, freeing the memory
 * allocated for each node.
 *
 * @param queue Pointer to the bspan queue to be freed.
 */
void free_bspan_queue(bspan_queue_t *queue);

#endif // !BSPAN_QUEUE_H
