#!/usr/bin/env python3
"""
Bitmap Allocators Benchmark Suite
Entry point for running all allocator tests with optional comparison report
"""

import os
import sys

# Add current directory to path for relative imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_all import STRATEGIES, run_all, run_test


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
                "Exit",
            ],
        )

        if main_idx == 2:
            print("Bye.")
            return

        if main_idx == 1:
            from run_report import run_and_save_report

            report_path = run_and_save_report()
            print(f"\nReport saved to: {report_path}")
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
  --tui       - Launch interactive TUI
  --help      - Show this help message

Examples:
  python run.py                    # Open interactive TUI
  python run.py avl --demo         # Demo only for AVL Tree
  python run.py rb --stress        # Stress test only for Red-Black Tree
  python run.py bplustree --stress # Stress test for B+Tree (fastest!)
  python run.py all --demo         # Demo for all strategies
  python run.py --report           # Generate report-YYYYMMDD-HHMMSS.md

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
