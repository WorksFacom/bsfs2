"""Consistency validation runner."""

from __future__ import annotations

import random


class ValidatorRunner:
    def __init__(self, strategy_specs):
        self.strategy_specs = strategy_specs

    @staticmethod
    def _run_validation_workload(
        allocator_class,
        bitmap_size,
        blocks_per_alloc,
        num_operations,
        seed,
    ):
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

    def run(self, bitmap_size=100000, blocks_per_alloc=5, num_operations=10000, seed=42):
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
        for key, allocator_class, name in self.strategy_specs:
            state = self._run_validation_workload(
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
