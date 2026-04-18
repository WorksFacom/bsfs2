#!/usr/bin/env python3
"""
Test runner for all bitmap allocators.
Supports running individual or all strategies.
"""

import random
import sys
import time
from statistics import median

from allocators import (
    BitmapAVL,
    BitmapRB,
    BitmapHeap,
    BitmapBTree,
    BitmapBPlusTree,
    BitmapHybridHeapRB,
    BitmapSegmentTree,
    BitmapTwoLevelIndex,
    BitmapTreap,
    BitmapBaseline,
)


STRATEGY_SPECS = [
    ("baseline", BitmapBaseline, "Baseline Sequential Scan"),
    ("avl", BitmapAVL, "AVL Tree"),
    ("rb", BitmapRB, "Red-Black Tree"),
    ("heap", BitmapHeap, "Min-Heap"),
    ("btree", BitmapBTree, "B-Tree"),
    ("bplustree", BitmapBPlusTree, "B+Tree"),
    ("hybrid", BitmapHybridHeapRB, "Hybrid Heap+RB"),
    ("segtree", BitmapSegmentTree, "Segment Tree"),
    ("twolevel", BitmapTwoLevelIndex, "Two-Level Index"),
    ("treap", BitmapTreap, "Treap"),
]
STRATEGIES = {
    key: (allocator_class, name) for key, allocator_class, name in STRATEGY_SPECS
}


class BitmapTester:
    """Universal bitmap allocator tester."""

    def __init__(self, allocator_class, name, bitmap_size=100000):
        self.allocator_class = allocator_class
        self.name = name
        self.bitmap_size = bitmap_size

    def demo(self):
        """Run demo mode."""
        print(f"\n{'=' * 74}")
        print(f"DEMO: {self.name} for Bitmap Block Allocation")
        print("=" * 74)

        allocator = self.allocator_class(self.bitmap_size)

        operations = [
            ("Allocate 3 blocks", lambda a: a.allocate_blocks(3)),
            ("Allocate 5 blocks", lambda a: a.allocate_blocks(5)),
            ("Deallocate blocks: [0, 1, 2]", lambda a: a.deallocate_blocks([0, 1, 2])),
            ("Allocate 2 blocks", lambda a: a.allocate_blocks(2)),
            ("Allocate 4 blocks", lambda a: a.allocate_blocks(4)),
            ("Deallocate blocks: [3, 6, 8]", lambda a: a.deallocate_blocks([3, 6, 8])),
            ("Allocate 2 blocks", lambda a: a.allocate_blocks(2)),
        ]

        for desc, op in operations:
            result = op(allocator)
            free_blocks = allocator.count_free_blocks()

            print(f"\n→ {desc}")
            if isinstance(result, list):
                print(f"  Allocated: {result}")

            bitmap_str = "".join(str(b) for b in allocator.bitmap[:50])
            print(f"  Bitmap: [{bitmap_str}...]")
            print(f"  Free blocks: {free_blocks:,}")

    def stress_test(
        self,
        blocks_per_alloc=5,
        num_operations=100000,
        show_progress=True,
        show_summary=True,
    ):
        """Run stress test and return metrics."""
        if show_summary:
            print(f"\n{'=' * 74}")
            print(f"STRESS TEST: {self.name} Multiple Block Allocation Performance")
            print("=" * 74)

            print("\nConfiguration:")
            print(f"  Bitmap size: {self.bitmap_size:,} blocks")
            print(f"  Blocks per allocation: {blocks_per_alloc}")
            print(f"  Total operations: {num_operations:,}")
            print("  Operation mix: 70% allocate / 30% deallocate")
            print("  Deallocation policy: random blocks among allocated")

        allocator = self.allocator_class(self.bitmap_size)
        rng = random.Random(42)

        alloc_times = []
        dealloc_times = []
        allocated_blocks = []
        progress_step = max(1, num_operations // 10)

        if show_progress:
            print("")

        for i in range(num_operations):
            if show_progress and i % progress_step == 0 and i > 0:
                print(
                    f"\rProgress: {i:,}/{num_operations:,} ({100 * i // num_operations}%)\033[K",
                    end="",
                    flush=True,
                )

            estimated_free_blocks = self.bitmap_size - len(allocated_blocks)
            should_allocate = rng.random() < 0.7 or not allocated_blocks

            # Avoid saturation lock at the very end of free space.
            if estimated_free_blocks <= blocks_per_alloc and allocated_blocks:
                should_allocate = False

            if should_allocate:
                t_start = time.perf_counter_ns()
                allocated = allocator.allocate_blocks(blocks_per_alloc)
                t_end = time.perf_counter_ns()

                if allocated:
                    alloc_times.append((t_end - t_start) / 1000.0)
                    allocated_blocks.extend(allocated)
            else:
                free_count = min(blocks_per_alloc, len(allocated_blocks))
                block_positions = sorted(
                    rng.sample(range(len(allocated_blocks)), free_count), reverse=True
                )
                blocks_to_free = [allocated_blocks[pos] for pos in block_positions]
                t_start = time.perf_counter_ns()
                success = allocator.deallocate_blocks(blocks_to_free)
                t_end = time.perf_counter_ns()

                if success:
                    dealloc_times.append((t_end - t_start) / 1000.0)
                    for pos in block_positions:
                        allocated_blocks.pop(pos)

        if show_progress:
            print(f"\rProgress: {num_operations:,}/{num_operations:,} (100%)\033[K")

        alloc_avg = sum(alloc_times) / len(alloc_times) if alloc_times else 0
        alloc_median = median(alloc_times) if alloc_times else 0
        alloc_p95 = (
            sorted(alloc_times)[int(len(alloc_times) * 0.95)] if alloc_times else 0
        )

        dealloc_avg = sum(dealloc_times) / len(dealloc_times) if dealloc_times else 0
        dealloc_median = median(dealloc_times) if dealloc_times else 0
        dealloc_p95 = (
            sorted(dealloc_times)[int(len(dealloc_times) * 0.95)]
            if dealloc_times
            else 0
        )

        total_time_ns = sum(alloc_times) + sum(dealloc_times)
        total_time_s = total_time_ns / 1e6
        total_ops = len(alloc_times) + len(dealloc_times)
        ops_per_sec = total_ops / total_time_s if total_time_s > 0 else 0
        avg_time_per_op = total_time_ns / total_ops if total_ops > 0 else 0

        free_blocks = allocator.count_free_blocks()
        occupied = self.bitmap_size - free_blocks

        results = {
            "name": self.name,
            "ops_per_sec": ops_per_sec,
            "total_time_s": total_time_s,
            "alloc_avg": alloc_avg,
            "alloc_median": alloc_median,
            "alloc_p95": alloc_p95,
            "dealloc_avg": dealloc_avg,
            "dealloc_median": dealloc_median,
            "dealloc_p95": dealloc_p95,
            "num_allocs": len(alloc_times),
            "num_deallocs": len(dealloc_times),
            "avg_time_per_op": avg_time_per_op,
            "occupied": occupied,
            "free_blocks": free_blocks,
        }

        if show_summary:
            print(f"\n{'=' * 74}")
            print("RESULTS")
            print("=" * 74)

            print("\nExecution:")
            print(f"  Total time: {total_time_s:.3f}s")
            print(f"  Operations/sec: {ops_per_sec:,.0f}")
            print(f"  Time per operation: {avg_time_per_op:.3f} µs")

            print(f"\nAllocation ({blocks_per_alloc} blocks):")
            print(f"  Average: {alloc_avg:.3f} µs")
            print(f"  Median:  {alloc_median:.3f} µs")
            print(f"  P95:     {alloc_p95:.3f} µs")
            print(f"  Total calls: {len(alloc_times):,}")

            print(f"\nDeallocation ({blocks_per_alloc} blocks):")
            print(f"  Average: {dealloc_avg:.3f} µs")
            print(f"  Median:  {dealloc_median:.3f} µs")
            print(f"  P95:     {dealloc_p95:.3f} µs")
            print(f"  Total calls: {len(dealloc_times):,}")

            print("\nFinal state:")
            print(f"  Occupied blocks: {occupied:,}")
            print(f"  Free blocks: {free_blocks:,}")

        return results


def run_test(strategy_name, demo=False, stress=False):
    """Run test for a specific strategy."""
    if strategy_name not in STRATEGIES:
        print(f"Unknown strategy: {strategy_name}")
        print(f"Available: {', '.join(STRATEGIES.keys())}")
        return

    allocator_class, name = STRATEGIES[strategy_name]
    tester = BitmapTester(allocator_class, name)

    if demo:
        tester.demo()
    if stress:
        tester.stress_test()
    if not demo and not stress:
        tester.demo()
        tester.stress_test()


def run_all(demo=False, stress=False):
    """Run tests for all strategies."""
    for _, allocator_class, name in STRATEGY_SPECS:
        tester = BitmapTester(allocator_class, name)
        if demo:
            tester.demo()
        if stress:
            tester.stress_test()
        if not demo and not stress:
            tester.demo()
            tester.stress_test()


if __name__ == "__main__":
    demo = "--demo" in sys.argv
    stress = "--stress" in sys.argv

    if not demo and not stress:
        demo = stress = True

    strategy = None
    for arg in sys.argv[1:]:
        if arg in STRATEGIES:
            strategy = arg
            break

    if strategy:
        run_test(strategy, demo, stress)
    else:
        run_all(demo, stress)
