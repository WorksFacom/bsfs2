#!/usr/bin/env python3
"""
Bitmap Allocators Benchmark Suite
Entry point for running all allocator tests with optional comparison report
"""

import os
import random
import sys

# Add current directory to path for relative imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_all import STRATEGY_SPECS, STRATEGIES, run_all, run_test


def _menu_select(title, options):
    """Render a simple numbered TUI selector."""
    while True:
        print("\n" + "=" * 72)
        print(title)
        print("=" * 72)
        for i, option in enumerate(options, 1):
            print(f"{i}. {option}")

        choice = input("\nSelect an option: ").strip()
        if choice.isdigit():
            idx = int(choice)
            if 1 <= idx <= len(options):
                return idx - 1
        print("Invalid option. Please try again.")


def run_tui():
    """Interactive terminal UI for selecting benchmark options."""
    strategy_labels = [f"all - All strategies"] + [f"{key} - {name}" for key, (_, name) in STRATEGIES.items()]
    mode_labels = ["Demo + Stress", "Demo only", "Stress only"]

    while True:
        main_idx = _menu_select(
            "Bitmap Allocators Benchmark Suite",
            [
                "Run benchmark",
                "Generate comparison report (.md)",
                "Run consistency validator",
                "Exit",
            ],
        )

        if main_idx == 3:
            print("Bye.")
            return

        if main_idx == 1:
            from run_report import run_and_save_report

            report_path = run_and_save_report()
            print(f"\nReport saved to: {report_path}")
            continue

        if main_idx == 2:
            passed = run_consistency_validator()
            if not passed:
                sys.exit(1)
            continue

        strategy_idx = _menu_select("Choose strategy", strategy_labels)
        mode_idx = _menu_select("Choose execution mode", mode_labels)

        demo = mode_idx in (0, 1)
        stress = mode_idx in (0, 2)
        strategy = "all" if strategy_idx == 0 else list(STRATEGIES.keys())[strategy_idx - 1]

        if strategy == "all":
            run_all(demo=demo, stress=stress)
        else:
            run_test(strategy, demo=demo, stress=stress)


def _run_validation_workload(
    allocator_class,
    bitmap_size,
    blocks_per_alloc,
    num_operations,
    seed,
):
    """Run deterministic mini-stress workload and return final state."""
    allocator = allocator_class(bitmap_size)
    rng = random.Random(seed)
    allocated_blocks = []

    for _ in range(num_operations):
        estimated_free_blocks = bitmap_size - len(allocated_blocks)
        should_allocate = rng.random() < 0.7 or not allocated_blocks

        if estimated_free_blocks <= blocks_per_alloc and allocated_blocks:
            should_allocate = False

        if should_allocate:
            allocated = allocator.allocate_blocks(blocks_per_alloc)
            if allocated:
                allocated_blocks.extend(allocated)
        else:
            free_count = min(blocks_per_alloc, len(allocated_blocks))
            block_positions = sorted(
                rng.sample(range(len(allocated_blocks)), free_count),
                reverse=True,
            )
            blocks_to_free = [allocated_blocks[pos] for pos in block_positions]
            success = allocator.deallocate_blocks(blocks_to_free)
            if success:
                for pos in block_positions:
                    allocated_blocks.pop(pos)

    bitmap = list(allocator.bitmap)
    occupied_count = sum(bitmap)
    free_count = allocator.count_free_blocks()
    tracked_occupied_count = len(allocated_blocks)
    bitmap_free_count = bitmap_size - occupied_count

    return {
        "bitmap": bitmap,
        "occupied_count": occupied_count,
        "free_count": free_count,
        "bitmap_free_count": bitmap_free_count,
        "tracked_occupied_count": tracked_occupied_count,
    }


def run_consistency_validator():
    """Validate that all allocators end with the same bitmap on fixed workload."""
    bitmap_size = 100000
    blocks_per_alloc = 5
    num_operations = 10000  # 10% of default stress load (100k)
    seed = 42

    print("\n" + "=" * 80)
    print("BITMAP ALLOCATORS - CONSISTENCY VALIDATOR")
    print("=" * 80)
    print("\nFixed workload:")
    print(f"  Bitmap size: {bitmap_size:,} blocks")
    print(f"  Blocks per allocation: {blocks_per_alloc}")
    print(f"  Operations: {num_operations:,} (10% of stress)")
    print(f"  RNG seed: {seed}")
    print("")

    results = []
    for key, allocator_class, name in STRATEGY_SPECS:
        state = _run_validation_workload(
            allocator_class=allocator_class,
            bitmap_size=bitmap_size,
            blocks_per_alloc=blocks_per_alloc,
            num_operations=num_operations,
            seed=seed,
        )
        results.append((key, name, state))
        print(f"  ✓ {name}")

    baseline_key, baseline_name, baseline_state = results[0]
    baseline_bitmap = baseline_state["bitmap"]

    divergence_messages = []
    consistency_messages = []

    for key, name, state in results:
        if state["free_count"] != state["bitmap_free_count"]:
            consistency_messages.append(
                f"- {name}: count_free_blocks={state['free_count']} != bitmap_free={state['bitmap_free_count']}"
            )
        if state["tracked_occupied_count"] != state["occupied_count"]:
            consistency_messages.append(
                f"- {name}: tracked_occupied={state['tracked_occupied_count']} != bitmap_occupied={state['occupied_count']}"
            )

        if key == baseline_key:
            continue

        if state["bitmap"] != baseline_bitmap:
            mismatch_count = sum(
                1 for a, b in zip(baseline_bitmap, state["bitmap"]) if a != b
            )
            first_mismatch_idx = next(
                i for i, (a, b) in enumerate(zip(baseline_bitmap, state["bitmap"])) if a != b
            )
            divergence_messages.append(
                f"- {name} divergiu de {baseline_name}: {mismatch_count} posições diferentes (primeira em idx={first_mismatch_idx})"
            )

    print("\n" + "-" * 80)
    if consistency_messages:
        print("INTERNAL CONSISTENCY CHECKS: FAILED")
        for message in consistency_messages:
            print(message)
    else:
        print("INTERNAL CONSISTENCY CHECKS: OK")

    if divergence_messages:
        print("BITMAP COMPARISON: FAILED")
        for message in divergence_messages:
            print(message)
        print("\nValidator result: FAIL")
        return False

    print("BITMAP COMPARISON: OK")
    print("Validator result: PASS")
    return not consistency_messages


def print_usage():
    """Print usage information"""
    print("""
Bitmap Allocators Benchmark Suite
==================================

Usage:
  python run.py                  # Launch interactive TUI
  python run.py [STRATEGY] [OPTIONS]

Strategies:
  avl         - AVL Tree
  rb          - Red-Black Tree
  heap        - Min-Heap
  btree       - B-Tree
  bplustree   - B+Tree
  hybrid      - Hybrid Heap+RB
  segtree     - Segment Tree
  twolevel    - Two-Level Index
  treap       - Treap
  baseline    - Baseline Sequential Scan
  all         - Run all strategies (default)

Options:
  --demo      - Run demo mode only
  --stress    - Run stress test only
  --report    - Run all strategies and generate markdown report file
  --validate  - Run fixed-workload validator across all allocators
  --tui       - Launch interactive TUI
  --help      - Show this help message

Examples:
  python run.py                    # Open interactive TUI
  python run.py avl --demo         # Demo only for AVL Tree
  python run.py rb --stress        # Stress test only for Red-Black Tree
  python run.py bplustree --stress # Stress test for B+Tree (fastest!)
  python run.py all --demo         # Demo for all strategies
  python run.py --report           # Generate report-YYYYMMDD-HHMMSS.md
  python run.py --validate         # Verify all allocators end with same bitmap

Results:
  The tests measure:
    - Time to allocate N blocks
    - Time to deallocate N blocks
    - Total operations per second
    - Memory fragmentation patterns

Performance Notes:
  - B+Tree, Hybrid, RB, Segment Tree, Two-Level Index, Treap
  - AVL Tree, Heap, B-Tree
""")


if __name__ == "__main__":
    if "--help" in sys.argv or "-h" in sys.argv:
        print_usage()
        sys.exit(0)

    if "--tui" in sys.argv or (len(sys.argv) == 1 and sys.stdin.isatty()):
        run_tui()
        sys.exit(0)
    
    # Check for report flag first
    if "--report" in sys.argv:
        from run_report import run_and_save_report

        report_path = run_and_save_report()
        print(f"\nReport saved to: {report_path}")
        sys.exit(0)

    if "--validate" in sys.argv:
        passed = run_consistency_validator()
        sys.exit(0 if passed else 1)
    
    demo = "--demo" in sys.argv
    stress = "--stress" in sys.argv
    
    # Default to both if no test type specified
    if not demo and not stress:
        demo = stress = True
    
    # Check for specific strategy
    strategy = None
    for arg in sys.argv[1:]:
        if arg in STRATEGIES:
            strategy = arg
            break
    
    if strategy:
        run_test(strategy, demo, stress)
    else:
        run_all(demo, stress)
