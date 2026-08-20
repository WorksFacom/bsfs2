/**
 * @file alloc_heap.c
 * @brief Min-heap of recycled blocks plus a sequential cursor.
 *
 * Selected by `make BSFS_ALLOC=heap`. Port of the `heap` strategy from
 * bitmap_test_c/src/allocators/scatter/heap.c ("Min-Heap" in the benchmarks).
 *
 * The idea is that the two kinds of free block deserve different structures.
 * Blocks the filesystem has never handed out form one long run at the end, and a
 * cursor describes that run in a single word. Blocks that were handed out and
 * later freed are scattered, and those go in a min-heap. Allocating is then just
 * `min(heap top, cursor)`, which is the smallest free block on the device - the
 * same block the linear scan would have found, without the scan.
 *
 * ## Where this differs from the userspace prototype
 *
 * The prototype starts from an empty bitmap, so `next_fresh = 0` and "every free
 * block below the cursor is in the heap" holds trivially. A mounted filesystem
 * starts from a bitmap full of holes, and preloading the heap with them would
 * cost 4 bytes per free block at mount time - 262 MB on a 250 GiB filesystem,
 * before the first write.
 *
 * So the cursor here *skips allocated blocks* instead of just counting up:
 *
 *   alloc()      advance the cursor past occupied bits
 *                take min(heap top, cursor)
 *   freed(b)     push b only if b < cursor
 *
 * which preserves the same invariant - every free block below the cursor is in
 * the heap - starting from an empty heap at mount, because the cursor only ever
 * moves across bits that are *not* free. Blocks freed ahead of the cursor need
 * no bookkeeping at all; the cursor will reach them. The heap therefore holds
 * only what this mount has freed behind itself, not the whole filesystem.
 *
 * Because the cursor never moves backwards, the scanning it does costs O(n) once
 * over the lifetime of a mount, not O(n) per allocation as the linear scan does.
 *
 * ## What is not here
 *
 * Nothing persists. The heap is rebuilt implicitly on the next mount (cursor
 * back to data_block_start, heap empty), and the bitmap in the buffer heads
 * stays the only record of what is allocated. That also means a crash costs
 * nothing but a slower first pass.
 */
#include "../include/alloc_strategy.h"
#include "../include/bitmap.h"
#include <linux/mm.h>
#include <linux/slab.h>

#define HEAP_INITIAL_CAP 64

/**
 * @brief In-memory state of the heap strategy, hung off sbi->alloc_private.
 *
 * @var heap       recycled block indices, a min-heap; kvmalloc'd, never on disk
 * @var next_fresh lowest block the cursor has not passed yet. Every free block
 *                 below it is in the heap; everything at or above it is still to
 *                 be discovered by scanning.
 * @var lossy      set when a push could not grow the array, which leaves blocks
 *                 reachable only by a full scan. alloc() falls back to one
 *                 rather than lose them.
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
 * Records a freed block.
 *
 * The array is grown with kvmalloc rather than kmalloc because on a filesystem
 * that is emptied in one go it holds one entry per freed block, which outgrows
 * the slab allocator's comfortable sizes long before it outgrows memory.
 *
 * A failed growth must not drop the block on the floor: its bit is already
 * clear, so nothing else would ever point at it again. The heap is marked
 * incomplete instead, and alloc() pays for a scan when it runs dry.
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
 * Nothing is preloaded: at mount every free block is at or above the cursor by
 * definition, so the heap starts empty and stays proportional to what this mount
 * frees rather than to the size of the filesystem.
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
 * Last resort: walk the whole bitmap.
 *
 * Only reachable after a failed heap growth, when a block may be free without
 * being recorded anywhere. Correctness net, never the normal path - the counter
 * exists so a test can insist it stays at zero.
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
 * Hands out the lowest free block: min(heap top, cursor).
 *
 * The heap only ever holds blocks freed behind the cursor, so its top is below
 * next_fresh whenever it exists - but the two are compared rather than assumed,
 * since the cursor may have been dragged past a block pushed earlier.
 *
 * A block popped from the heap is supposed to be free. If it is not, something
 * has drifted out of sync and handing it out would give one block to two owners,
 * so it is dropped instead. It comes back on its next real free.
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
 * At or above the cursor there is nothing to record: the cursor has not been
 * there yet and will find the block on its own. That is also what keeps the heap
 * small - a filesystem filled and emptied in order never pushes anything.
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
