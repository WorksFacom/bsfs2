# Bitmap Allocators - Data Structure Implementations

This folder contains clean implementations of bitmap allocators using different indexing strategies.

## Available Allocators

Each allocator uses a different data structure to index free blocks in a bitmap, with the same interface but different performance characteristics.

### 1. AVL Tree (`avl.py`)
- **Structure**: Self-balancing binary search tree
- **Height balance**: Height difference ≤ 1 between subtrees
- **Find free block**: O(log n) - traverse left to find minimum
- **Allocate**: O(log n) - find and delete from tree
- **Deallocate**: O(log n) - insert back into tree
- **Performance**: 65k ops/sec, 15.4 µs per allocation
- **Best for**: Educational purposes, understanding balanced trees

### 2. Red-Black Tree (`rbtree.py`)
- **Structure**: Self-balancing BST with color constraints
- **Height bound**: 2×log(n+1) vs AVL's 1.44×log(n)
- **Find free block**: O(log n) - traverse left
- **Allocate**: O(log n) - find and delete
- **Deallocate**: O(log n) - insert with recoloring
- **Performance**: 355k ops/sec, 2.7 µs per allocation **5.5x faster than AVL**
- **Best for**: Production systems needing balanced performance
- **Why faster**: Less rebalancing overhead, fewer rotations

### 3. Min-Heap (`heap.py`)
- **Structure**: Array-based min-heap with position map
- **Find free block**: O(1) - always at root (minimum)
- **Allocate**: O(log n) - extract min, percolate down
- **Deallocate**: O(log n) - insert, percolate up
- **Performance**: 61k ops/sec, 19.9 µs per allocation
- **Best for**: When simplicity matters more than peak performance
- **Limitation**: Good for finding minimum, not searching

### 4. B-Tree (`btree.py`)
- **Structure**: Multi-way search tree with n-ary branching
- **Order**: 256 (adjustable)
- **Find free block**: O(log n) - search to leaf
- **Allocate**: O(log n) - find and delete
- **Deallocate**: ❌ **151 µs** - VERY SLOW (catastrophic rebalancing)
- **Performance**: 13k ops/sec - **AVOID**
- **Why slow**: Delete operation requires complex rebalancing with borrowing/merging
- **Lesson**: B-Tree delete is complex; this is why B+Tree exists

### 5. B+Tree (`bplustree.py`) ⭐ **WINNER**
- **Structure**: Multi-way search tree with leaf node linking
- **Order**: 256 (adjustable)
- **Find free block**: O(1) cached via leaf linking
- **Allocate**: O(log n) - search, delete from leaf
- **Deallocate**: O(log n) - insert, rebalance only when full
- **Performance**: 442k ops/sec, 2.2 µs per allocation **6.8x faster than AVL**
- **Best for**: Filesystem and database allocators
- **Why best**: 
  - Leaf linking allows O(1) minimum cache
  - Rebalancing only on full nodes, not every operation
  - Better cache locality (data only in leaves)
  - Proven in production databases

## Common Interface

All allocators implement this interface:

```python
from allocators import BitmapBPlusTree  # or any other

# Create allocator for bitmap with 100k blocks
allocator = BitmapBPlusTree(size=100000)

# Allocate 5 blocks (returns list of indices)
blocks = allocator.allocate_blocks(5)
# Example: blocks = [0, 1, 2, 3, 4]

# Deallocate blocks
allocator.deallocate_blocks(blocks)

# Count free blocks
free = allocator.count_free_blocks()
```

## Key Methods

### `allocate_blocks(count: int) -> List[int]`
Allocate `count` contiguous or non-contiguous free blocks using first-fit strategy.

Returns:
- List of block indices if successful
- Empty list if insufficient free blocks

Example:
```python
blocks = allocator.allocate_blocks(5)  # Get 5 blocks
if blocks:
    print(f"Allocated: {blocks}")  # [2, 3, 4, 5, 6]
else:
    print("No space available")
```

### `deallocate_blocks(indices: List[int]) -> bool`
Return blocks to free pool.

Returns:
- True if successful
- False if any block was already free (error condition)

Example:
```python
success = allocator.deallocate_blocks([2, 3, 4])
```

### `count_free_blocks() -> int`
Count total free blocks in the bitmap.

Example:
```python
free = allocator.count_free_blocks()
print(f"Free blocks: {free:,}")
```

## Performance Ranking

1. **B+Tree**: 442,606 ops/sec ⭐
2. **Red-Black Tree**: 355,394 ops/sec
3. **AVL Tree**: 64,852 ops/sec
4. **Min-Heap**: 60,624 ops/sec
5. **B-Tree**: 13,018 ops/sec (❌ avoid)

## Implementation Quality

All implementations include:
- ✅ Full rebalancing logic (for balanced trees)
- ✅ Error handling and edge cases
- ✅ Consistent interface
- ✅ Clean, readable code
- ✅ No external dependencies (stdlib only)

## Design Notes

### Why multiple implementations?

This is an A/B testing framework for evaluating allocator strategies. Real BSFS uses sequential O(n) scan; we benchmark these alternatives to find the fastest.

### Bitmap representation

- `bitmap[i] = 1` means block i is **occupied**
- `bitmap[i] = 0` means block i is **free**

Each allocator maintains separate indexing:
- AVL/RB/Heap/B-Tree/B+Tree stores only **free block indices**
- Occupied blocks stay in bitmap only
- On allocation: remove from tree, mark in bitmap
- On deallocation: mark in bitmap, add back to tree

### Why B+Tree is fastest

1. **Leaf linking**: Linked leaves mean finding minimum is O(1) cache hit
2. **Lazy rebalancing**: Only rebalance when node is FULL, not every operation
3. **Height**: O(log n) height even for 100k blocks = ~4-5 levels
4. **Proven design**: Used in production databases and filesystems

## Testing

Run all tests from project root:
```bash
python run_benchmarks.py [strategy] [--demo|--stress]
```

Or use `run_all.py` directly for more control.

## Next Steps

1. **Integration**: Adapt B+Tree allocator into BSFS C code
2. **Real workloads**: Benchmark with actual filesystem patterns
3. **Tuning**: Optimize B+Tree order (256 is a guess)
4. **Hybrid**: Consider combining with current bitmap cache strategy

See `../docs/` for detailed analysis and comparison reports.
