#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics
from typing import Any, Iterable

from bench.orchestration.report import write_report


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate BSFS JSONL results and render an HTML report"
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
    manifest: dict[str, Any] = {}
    manifest_path = run_dir / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for failure in manifest.get("failures", []):
            failures.append({"source": "orchestrator", **failure})
    write_csv(output / "summary.csv", summary)
    write_csv(output / "concurrency-scaling.csv", scaling)
    write_csv(output / "failed-phases.csv", failures)
    write_report(
        output / "report.html",
        run_dir=run_dir,
        records=records,
        summary=summary,
        scaling=scaling,
        failures=failures,
        skipped=skipped,
        manifest=manifest,
    )
    print(
        f"analyzed {len(records)} records: {len(summary)} groups, "
        f"{len(failures)} failed phases; report: {output / 'report.html'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
