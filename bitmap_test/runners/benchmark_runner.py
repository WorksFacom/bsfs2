"""Benchmark execution runner."""

from __future__ import annotations

from scenarios import StressScenario


class BenchmarkRunner:
    def __init__(self, allocator_class, allocator_name, bitmap_size=100000):
        self.allocator_class = allocator_class
        self.allocator_name = allocator_name
        self.bitmap_size = bitmap_size

    def run_demo(self):
        print(f"\n{'=' * 74}")
        print(f"DEMO: {self.allocator_name} for Bitmap Block Allocation")
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

    def run_stress(
        self,
        blocks_per_alloc=5,
        num_operations=100000,
        show_progress=True,
        show_summary=True,
    ):
        scenario = StressScenario(
            blocks_per_alloc=blocks_per_alloc,
            num_operations=num_operations,
            seed=42,
        )
        return scenario.run(
            self.allocator_class,
            self.allocator_name,
            self.bitmap_size,
            show_progress=show_progress,
            show_summary=show_summary,
        )
