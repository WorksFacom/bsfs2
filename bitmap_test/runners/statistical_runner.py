"""Statistical benchmark runner for repeated seeded experiments."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from core.io_csv import write_csv
from core.stats import generate_seeds
from scenarios import AllocationOnlyScenario, ParetoScenario, MultisizeParetoScenario


RUN_FIELDS = [
    "workload_label",
    "workload_file_size_mib",
    "workload_ops",
    "seed",
    "mode",
    "strategy_key",
    "strategy_name",
    "status",
    "error",
    "bitmap_size",
    "target_blocks",
    "blocks_per_file",
    "num_operations",
    "ops_per_sec",
    "total_time_s",
    "alloc_avg",
    "dealloc_avg",
    "num_allocs",
    "num_deallocs",
    "avg_time_per_op",
    "occupied",
    "free_blocks",
    "fragmentation",
]

RUN_FIELD_HEADERS = {
    "workload_label": "workload[label]",
    "workload_file_size_mib": "workload_file_size_mib[MiB]",
    "workload_ops": "workload_ops[count]",
    "seed": "seed[id]",
    "mode": "mode[label]",
    "strategy_key": "strategy_key[label]",
    "strategy_name": "strategy_name[label]",
    "status": "status[label]",
    "error": "error[text]",
    "bitmap_size": "bitmap_size[blocks]",
    "target_blocks": "target_blocks[blocks]",
    "blocks_per_file": "blocks_per_file[blocks/file]",
    "num_operations": "num_operations[ops]",
    "ops_per_sec": "ops_per_sec[ops/s]",
    "total_time_s": "total_time[s]",
    "alloc_avg": "alloc_avg[us]",
    "dealloc_avg": "dealloc_avg[us]",
    "num_allocs": "num_allocs[calls]",
    "num_deallocs": "num_deallocs[calls]",
    "avg_time_per_op": "avg_time_per_op[us/op]",
    "occupied": "occupied[blocks]",
    "free_blocks": "free_blocks[blocks]",
    "fragmentation": "fragmentation[ratio]",
}

class StatisticalRunner:
    def __init__(self, strategy_specs, config):
        self.strategy_specs = strategy_specs
        self.config = config

    def _build_scenario(self, seed):
        blocks_per_alloc = self.config.blocks_per_file
        if self.config.mode == "allocation_only":
            return AllocationOnlyScenario(
                blocks_per_alloc=blocks_per_alloc,
                num_operations=self.config.resolved_num_operations,
            )
        if self.config.mode == "multisize_pareto":
            return MultisizeParetoScenario(
                num_operations=self.config.resolved_num_operations,
                seed=seed,
            )
        return ParetoScenario(
            blocks_per_alloc=blocks_per_alloc,
            num_operations=self.config.resolved_num_operations,
            seed=seed,
        )

    def _row_from_metrics(self, seed, strategy_key, strategy_name, metrics, status="ok", error=""):
        return {
            "seed": seed,
            "workload_label": self.config.workload_label,
            "workload_file_size_mib": self.config.file_size_mib,
            "workload_ops": self.config.resolved_num_operations,
            "mode": self.config.mode,
            "strategy_key": strategy_key,
            "strategy_name": strategy_name,
            "status": status,
            "error": error,
            "bitmap_size": self.config.bitmap_size,
            "target_blocks": self.config.target_blocks,
            "blocks_per_file": self.config.blocks_per_file,
            "num_operations": self.config.resolved_num_operations,
            "ops_per_sec": metrics.get("ops_per_sec", 0.0),
            "total_time_s": metrics.get("total_time_s", 0.0),
            "alloc_avg": metrics.get("alloc_avg", 0.0),
            "dealloc_avg": metrics.get("dealloc_avg", 0.0),
            "num_allocs": metrics.get("num_allocs", 0),
            "num_deallocs": metrics.get("num_deallocs", 0),
            "avg_time_per_op": metrics.get("avg_time_per_op", 0.0),
            "occupied": metrics.get("occupied", 0),
            "free_blocks": metrics.get("free_blocks", 0),
            "fragmentation": metrics.get("fragmentation", 0.0),
        }

    def run(self, strategy_keys=None):
        self.config.validate()
        output_dir = Path(self.config.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

        selected = set(strategy_keys) if strategy_keys else None
        strategy_specs = [
            spec for spec in self.strategy_specs if selected is None or spec[0] in selected
        ]
        if not strategy_specs:
            raise ValueError("No strategies selected for statistical benchmark")

        seeds = generate_seeds(self.config.num_seeds, self.config.seed_base)
        run_rows = []

        total_runs = len(seeds) * len(strategy_specs)
        done = 0
        print("\n" + "=" * 80)
        print("BITMAP ALLOCATORS - STATISTICAL BENCHMARK")
        print("=" * 80)
        print(f"Workload: {self.config.workload_label}")
        print(f"Mode: {self.config.mode}")
        print(f"Strategies: {len(strategy_specs)}")
        print(f"Seeds: {len(seeds)}")
        print(f"Bitmap size: {self.config.bitmap_size:,} blocks")
        print(f"Blocks per allocation (file): {self.config.blocks_per_file}")
        print(f"Operations per run: {self.config.resolved_num_operations:,}")
        print("")

        for seed in seeds:
            for strategy_key, allocator_class, strategy_name in strategy_specs:
                try:
                    scenario = self._build_scenario(seed)
                    metrics = scenario.run(
                        allocator_class=allocator_class,
                        allocator_name=strategy_name,
                        bitmap_size=self.config.bitmap_size,
                        show_progress=False,
                        show_summary=False,
                    )
                    run_rows.append(
                        self._row_from_metrics(
                            seed,
                            strategy_key,
                            strategy_name,
                            metrics,
                            status="ok",
                            error="",
                        )
                    )
                except Exception as exc:
                    print(
                        f"ERROR seed={seed} strategy={strategy_key}: {exc.__class__.__name__}: {exc}"
                    )
                    run_rows.append(
                        self._row_from_metrics(
                            seed,
                            strategy_key,
                            strategy_name,
                            metrics={},
                            status="error",
                            error=f"{exc.__class__.__name__}: {exc}",
                        )
                    )
                done += 1
                if done % max(1, total_runs // 20) == 0:
                    print(f"Progress: {done:,}/{total_runs:,}")

        mode_suffix = self.config.mode
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        runs_path = write_csv(
            output_dir / f"benchmark_runs_{mode_suffix}_{timestamp}.csv",
            run_rows,
            RUN_FIELDS,
            header_labels=RUN_FIELD_HEADERS,
        )
        failed_runs = sum(1 for row in run_rows if row["status"] != "ok")

        return {
            "workload_label": self.config.workload_label,
            "mode": self.config.mode,
            "runs_path": str(runs_path),
            "runs": len(run_rows),
            "failed_runs": failed_runs,
            "strategies": len(strategy_specs),
            "seeds": len(seeds),
        }
