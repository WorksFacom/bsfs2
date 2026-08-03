#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
import os
from pathlib import Path
import statistics
from typing import Any, Iterable


GROUP_FIELDS = ("filesystem", "scenario", "phase", "worker_count", "worker_id")
METRICS = (
    "durable_throughput_mib_s",
    "durable_iops",
    "throughput_mib_s",
    "iops",
    "elapsed_ns",
    "operation_path_ns",
    "final_flush_ns",
    "durable_total_ns",
    "latency_p50_ns",
    "latency_p95_ns",
    "latency_p99_ns",
    "latency_max_ns",
    "device_sectors_read",
    "device_sectors_written",
    "reclaim_blocks_reclaimed",
)


def load_records(path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    """An interrupted run leaves a truncated final line; report it rather than
    refusing to analyze everything that did complete."""
    records: list[dict[str, Any]] = []
    skipped: list[str] = []
    with path.open("r", encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as error:
                skipped.append(f"{path.name}:{number}: {error}")
    return records, skipped


def numeric(values: Iterable[Any]) -> list[float]:
    return [
        float(value)
        for value in values
        if value is not None and isinstance(value, (int, float))
    ]


def summarize(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {
            "count": 0,
            "median": None,
            "minimum": None,
            "maximum": None,
            "mean": None,
            "stddev": None,
        }
    return {
        "count": len(values),
        "median": statistics.median(values),
        "minimum": min(values),
        "maximum": max(values),
        "mean": statistics.fmean(values),
        "stddev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def derive_summary(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        if record.get("validation_status") != "passed":
            continue
        grouped[tuple(record.get(field) for field in GROUP_FIELDS)].append(record)

    rows: list[dict[str, Any]] = []
    for key, group in sorted(grouped.items(), key=lambda item: str(item[0])):
        row = dict(zip(GROUP_FIELDS, key, strict=True))
        row["repetitions"] = len(group)
        for metric in METRICS:
            summary = summarize(numeric(record.get(metric) for record in group))
            for statistic, value in summary.items():
                row[f"{metric}_{statistic}"] = value
        amplifications = []
        for record in group:
            # without a flush inside the phase the device counters describe
            # whatever writeback happened to land, not this phase's writes
            if not record.get("has_durable_window"):
                continue
            logical = record.get("logical_bytes") or 0
            written_sectors = record.get("device_sectors_written")
            sectors = written_sectors * 512 if written_sectors is not None else 0
            if logical and written_sectors is not None:
                amplifications.append(sectors / logical)
        amplification = summarize(amplifications)
        for statistic, value in amplification.items():
            row[f"write_amplification_{statistic}"] = value
        rows.append(row)
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = sorted({key for row in rows for key in row})
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def derive_scaling(summary: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_key: dict[tuple[str, str, str], dict[int, dict[str, Any]]] = defaultdict(
        dict
    )
    for row in summary:
        if not str(row["scenario"]).startswith("concurrency-"):
            continue
        if row.get("worker_id") is not None:
            continue
        key = (str(row["filesystem"]), str(row["scenario"]), str(row["phase"]))
        by_key[key][int(row["worker_count"])] = row
    output: list[dict[str, Any]] = []
    for key, workers in sorted(by_key.items()):
        if 1 not in workers or 4 not in workers:
            continue
        one = workers[1].get("durable_throughput_mib_s_median")
        four = workers[4].get("durable_throughput_mib_s_median")
        if not one or four is None:
            continue
        output.append(
            {
                "filesystem": key[0],
                "scenario": key[1],
                "phase": key[2],
                "one_worker_mib_s": one,
                "four_worker_mib_s": four,
                "scaling_ratio": float(four) / float(one),
            }
        )
    return output


def safe_name(value: str) -> str:
    return "".join(
        char if char.isalnum() or char in "-_" else "_" for char in value
    )


def charts(
    summary: list[dict[str, Any]],
    output: Path,
    failures: list[dict[str, Any]] | None = None,
) -> None:
    os.environ.setdefault("MPLCONFIGDIR", str(output / ".matplotlib"))
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError(
            "chart generation requires matplotlib and numpy"
        ) from error

    chart_specs = (
        ("durable_throughput_mib_s", "MiB/s including flush", "Higher is better"),
        ("durable_iops", "operations/s including flush", "Higher is better"),
        ("durable_total_ns", "nanoseconds", "Lower is better"),
        ("latency_p99_ns", "nanoseconds", "Lower is better"),
        ("write_amplification", "device bytes / logical bytes", "Lower is better"),
    )
    # a filesystem absent from a chart because it failed must not read as
    # "not run", so name the failures under the axis
    failed_by_phase: dict[tuple[str, str], set[str]] = defaultdict(set)
    for failure in failures or []:
        key = (str(failure.get("scenario")), str(failure.get("phase")))
        failed_by_phase[key].add(str(failure.get("filesystem")))

    phase_groups: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in summary:
        phase_groups[(str(row["scenario"]), str(row["phase"]))].append(row)

    for (scenario, phase), rows in sorted(phase_groups.items()):
        for metric, label, direction in chart_specs:
            median_field = f"{metric}_median"
            if not any(row.get(median_field) is not None for row in rows):
                continue
            chart_rows = [
                row for row in rows if row.get(median_field) is not None
            ]
            chart_rows.sort(
                key=lambda row: (
                    str(row["filesystem"]),
                    int(row["worker_count"]),
                    -1 if row.get("worker_id") is None else int(row["worker_id"]),
                )
            )
            labels = [
                (
                    f"{row['filesystem']} (w={row['worker_count']}, "
                    f"id={row['worker_id']})"
                    if row.get("worker_id") is not None
                    else f"{row['filesystem']} (w={row['worker_count']}, aggregate)"
                )
                for row in chart_rows
            ]
            medians = [float(row[median_field]) for row in chart_rows]
            minimums = [
                float(row[f"{metric}_minimum"]) for row in chart_rows
            ]
            maximums = [
                float(row[f"{metric}_maximum"]) for row in chart_rows
            ]
            lower = [median - minimum for median, minimum in zip(medians, minimums)]
            upper = [maximum - median for median, maximum in zip(maximums, medians)]
            figure, axis = plt.subplots(figsize=(max(7, len(labels) * 1.3), 4.8))
            positions = list(range(len(labels)))
            axis.bar(positions, medians, color="#3b6ea8")
            axis.errorbar(
                positions,
                medians,
                yerr=[lower, upper],
                fmt="none",
                ecolor="black",
                capsize=4,
            )
            axis.set_xticks(positions, labels, rotation=30, ha="right")
            axis.set_ylabel(f"{label} — {direction}")
            axis.set_title(f"{scenario}: {phase}")
            axis.grid(axis="y", alpha=0.25)
            failed = sorted(failed_by_phase.get((scenario, phase), set()))
            if failed:
                axis.set_xlabel(f"failed, not shown: {', '.join(failed)}")
            figure.tight_layout()
            stem = safe_name(f"{scenario}-{phase}-{metric}")
            for suffix in ("png", "svg"):
                figure.savefig(output / f"{stem}.{suffix}", dpi=180)
            plt.close(figure)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate BSFS JSONL results and render publication charts"
    )
    parser.add_argument("run_dir", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run_dir = args.run_dir.resolve()
    raw = run_dir / "raw" / "results.jsonl"
    if not raw.exists():
        raise SystemExit(f"results not found: {raw}")
    output = run_dir / "derived"
    output.mkdir(parents=True, exist_ok=True)
    records, skipped = load_records(raw)
    for message in skipped:
        print(f"warning: skipped unreadable result line: {message}")
    summary = derive_summary(records)
    scaling = derive_scaling(summary)
    failures: list[dict[str, Any]] = [
        record
        for record in records
        if record.get("validation_status") != "passed"
    ]
    manifest_path = run_dir / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for failure in manifest.get("failures", []):
            failures.append({"source": "orchestrator", **failure})
    write_csv(output / "summary.csv", summary)
    write_csv(output / "concurrency-scaling.csv", scaling)
    write_csv(output / "failed-phases.csv", failures)
    charts(summary, output, failures)
    print(
        f"analyzed {len(records)} records: {len(summary)} groups, "
        f"{len(failures)} failed phases"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
