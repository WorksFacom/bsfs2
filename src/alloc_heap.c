/**
 * @file alloc_heap.c
 * @brief min-heap of recycled blocks plus a sequential cursor.
 *
 * `make BSFS_ALLOC=heap`. port of bitmap_test_c/src/allocators/scatter/heap.c ("Min-Heap" in the benchmarks).
 *
 * never-used blocks are one run at the end, so a cursor covers them. freed blocks are scattered and go
 * in a min-heap. alloc is `min(heap top, cursor)`, the same block a linear scan finds, minus the scan.
 *
 * ## Differences from the userspace prototype
 *
 * the prototype starts from an empty bitmap. a mounted fs is full of holes and preloading them costs
 * 4 bytes per free block, 262 MB on 250 GiB. so the cursor skips allocated bits instead of counting up:
 *
 *   alloc()      advance the cursor past occupied bits
 *                take min(heap top, cursor)
 *   freed(b)     push b only if b < cursor
 *
 * invariant: every free block below the cursor is in the heap. holds from an empty heap at mount since the
 * cursor only crosses used bits. blocks freed ahead of it need nothing, the cursor gets there. the cursor
 * never goes backwards, so scanning is O(n) per mount, not per alloc.
 *
 * ## Persistence
 *
 * none. next mount starts over (cursor at data_block_start, empty heap) and the bitmap stays the only
 * record, so a crash costs a slower first pass.
 */
#include "../include/alloc_strategy.h"
#include "../include/bitmap.h"
#include <linux/mm.h>
#include <linux/slab.h>

#define HEAP_INITIAL_CAP 64

/**
 * @brief heap strategy state, hung off sbi->alloc_private.
 *
 * @var heap       recycled block indices, kvmalloc'd min-heap
 * @var next_fresh lowest block the cursor hasn't passed. free blocks below it are in the heap
 * @var lossy      a push failed to grow the array, so some free blocks are only reachable by a full scan
 */
struct heap_alloc {
  __u32 *heap;
  __u32  len;
  __u32  cap;
  __u32  next_fresh;
  bool   lossy;
};

/* ---------------------------------------------------------------- heap ---- */

static void heap_sift_up(__u32 *heap, __u32 i) {
  while (i > 0) {
    __u32 parent = (i - 1) / 2;

    if (heap[parent] <= heap[i])
      break;

    swap(heap[parent], heap[i]);
    i = parent;
  }
}

static void heap_sift_down(__u32 *heap, __u32 len, __u32 i) {
  for (;;) {
    __u32 left = 2 * i + 1, right = 2 * i + 2, smallest = i;

    if (left < len && heap[left] < heap[smallest])
      smallest = left;

    if (right < len && heap[right] < heap[smallest])
      smallest = right;

    if (smallest == i)
      break;

    swap(heap[i], heap[smallest]);
    i = smallest;
  }
}

/*
 * kvmalloc since emptying a whole fs pushes one entry per block, way past comfortable slab sizes.
 * a failed grow can't just drop the block, its bit is already clear and nothing would find it again.
 * mark the heap lossy and let alloc() scan when it runs dry.
 */
// caller holds sbi->bitmap_lock
static void heap_push(struct super_block *sb, struct heap_alloc *h, __u32 block) {
  if (h->len == h->cap) {
    __u32  new_cap = h->cap ? h->cap * 2 : HEAP_INITIAL_CAP;
    __u32 *grown;

    grown = kvmalloc_array(new_cap, sizeof(*grown), GFP_NOFS);

    if (!grown) {
      h->lossy = true;
      pr_warn_ratelimited("no memory to grow the free-block heap (%u entries), falling back to scanning\n", h->len);
      return;
    }

    if (h->heap) {
      memcpy(grown, h->heap, (size_t)h->len * sizeof(*grown));
      kvfree(h->heap);
    }

    h->heap = grown;
    h->cap  = new_cap;
  }

  h->heap[h->len] = block;
  heap_sift_up(h->heap, h->len);
  h->len++;

  BSFS_STAT_MAX(sb, index_peak, (__u64)h->len);
}

static __u32 heap_pop(struct heap_alloc *h) {
  __u32 top = h->heap[0];

  h->len--;

  if (h->len > 0) {
    h->heap[0] = h->heap[h->len];
    heap_sift_down(h->heap, h->len, 0);
  }

  return top;
}

/* ------------------------------------------------------------ strategy ---- */

/*
 * nothing preloaded. at mount every free block is at or above the cursor, so the heap only grows
 * with what this mount frees.
 */
int bsfs_alloc_strategy_init(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  struct heap_alloc      *h;

  h = kzalloc(sizeof(*h), GFP_KERNEL);

  if (!h)
    return -ENOMEM;

  h->next_fresh = sbi->super->data_block_start;

  sbi->alloc_private = h;
  return 0;
}

void bsfs_alloc_strategy_destroy(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  struct heap_alloc      *h   = sbi->alloc_private;

  if (!h)
    return;

  kvfree(h->heap);
  kfree(h);
  sbi->alloc_private = NULL;
}

/*
 * full bitmap walk, only after a failed heap grow. tests check the counter stays at zero.
 */
// caller holds sbi->bitmap_lock
static __u32 heap_fallback_scan(struct super_block *sb) {
  struct bsfs_super_info *sbi = bsfs_sb(sb);
  __u32                   i;

  BSFS_STAT_INC(sb, fallback_scans);

  for (i = sbi->super->data_block_start; i < sbi->super->total_blocks; i++) {
    if (bsfs_is_bit_free(sb, i)) {
      bsfs_set_bit(sb, i);
      return i;
    }
  }

  return INVALID_BLOCK;
}

/*
 * lowest free block, min(heap top, cursor). compared rather than assumed since the cursor can get
 * dragged past a block pushed earlier.
 *
 * a popped block that isn't free means drift. handing it out would give it two owners, so drop it,
 * it comes back on its next real free.
 */
// caller holds sbi->bitmap_lock
__u32 bsfs_alloc_strategy_alloc(struct super_block *sb) {
  struct bsfs_super_info *sbi   = bsfs_sb(sb);
  struct heap_alloc      *h     = sbi->alloc_private;
  __u32                   total = sbi->super->total_blocks;

  while (h->next_fresh < total && !bsfs_is_bit_free(sb, h->next_fresh))
    h->next_fresh++;

  for (;;) {
    if (h->len == 0)
      break;

    if (h->next_fresh < total && h->heap[0] > h->next_fresh)
      break;

    __u32 block = heap_pop(h);

    if (!bsfs_is_bit_free(sb, block)) {
      pr_warn_ratelimited("heap held block %u which is already allocated, dropping it\n", block);
      continue;
    }

    bsfs_set_bit(sb, block);
    BSFS_STAT_INC(sb, from_index);
    return block;
  }

  if (h->next_fresh < total) {
    __u32 block = h->next_fresh++;

    bsfs_set_bit(sb, block);
    BSFS_STAT_INC(sb, from_cursor);
    return block;
  }

  if (h->lossy) {
    __u32 block = heap_fallback_scan(sb);

    if (block != INVALID_BLOCK) {
      BSFS_STAT_INC(sb, from_cursor);
      return block;
    }
  }

  return INVALID_BLOCK;
}

/*
 * at or above the cursor the scan will find it anyway. filling and emptying in order never pushes.
 */
// caller holds sbi->bitmap_lock
void bsfs_alloc_strategy_freed(struct super_block *sb, __u32 block) {
  struct heap_alloc *h = bsfs_sb(sb)->alloc_private;

  if (block >= h->next_fresh)
    return;

  heap_push(sb, h, block);
}

void bsfs_alloc_strategy_stats(struct super_block *sb, char *buf, size_t len) {
  struct heap_alloc *h = bsfs_sb(sb)->alloc_private;

  if (!h) {
    if (len)
      buf[0] = '\0';
    return;
  }

  snprintf(buf, len, "heap_len=%u heap_cap=%u aux_bytes=%zu cursor=%u lossy=%d", h->len, h->cap,
           (size_t)h->cap * sizeof(*h->heap), h->next_fresh, h->lossy ? 1 : 0);
}
