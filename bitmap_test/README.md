# Bitmap Allocators Test Suite

Complete, encapsulated test framework for comparing bitmap allocation strategies.

## Quick Start

```bash
# Open interactive TUI
python run.py

# Run specific strategy
python run.py bplustree --stress

# Run in demo mode only
python run.py all --demo

# Generate markdown report file
python run.py --report
```

## Structure

```
bitmap_test/
├── run.py                    # Main entry point
├── run_all.py                # Base runner (demo/stress)
├── run_report.py             # Comparison report generator
├── allocators/               # Data structure implementations
│   ├── avl.py
│   ├── rbtree.py
│   ├── heap.py
│   ├── btree.py
│   ├── bplustree.py
│   ├── __init__.py
│   └── README.md
└── README.md                 # This file
```

## Available Allocators

| Strategy | Speed | Notes |
|----------|-------|-------|
| **B+Tree** ⭐ | 442k ops/sec | Recommended - Best overall |
| Red-Black Tree | 355k ops/sec | Fast balanced tree |
| AVL Tree | 62k ops/sec | Highly balanced |
| Heap | 61k ops/sec | Simple, fast minimum |
| B-Tree | 13k ops/sec | Avoid - slow delete |

## Commands

```bash
# Open interactive TUI (recommended)
python run.py

# Specific strategy
python run.py bplustree
python run.py rb --demo
python run.py avl --stress

# Report output file (created in current directory)
python run.py --report
# => report-YYYYMMDD-HHMMSS.md

# Help
python run.py --help
```

## Usage in Code

```python
import sys
sys.path.insert(0, 'bitmap_test')

from allocators import BitmapBPlusTree

allocator = BitmapBPlusTree(size=100000)
blocks = allocator.allocate_blocks(5)
allocator.deallocate_blocks(blocks)
free = allocator.count_free_blocks()
```

See `allocators/README.md` for detailed allocator documentation.
