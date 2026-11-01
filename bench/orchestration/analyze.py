#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics
import sys
from typing import Any, Iterable

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from bench.orchestration.report import write_report
else:
    from .report import write_report


GROUP_FIELDS = (
    "filesystem",
    "scenario",
    "phase",
    "worker_count",
    "work_scale",
    "worker_id",
)
DURABLE_FIELDS = ("durable_throughput_mib_s", "durable_iops", "durable_total_ns")
METRICS = (
    "durable_throughput_mib_s",
    "durable_iops",
    "throughput_mib_s",
    "iops",
    "elapsed_ns",
    "operation_path_ns",
    "final_flush_ns",
    "wall_ns",
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
    """Skips and reports unparseable lines, since an interrupted run leaves a truncated last one."""
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


def without_false_durability(record: dict[str, Any]) -> dict[str, Any]:
    """Remove durable rates from schema 1 records for phases that did not flush."""
    if record.get("has_durable_window") is not False:
        return record
    return {**record, **dict.fromkeys(DURABLE_FIELDS)}


def with_work_scale(record: dict[str, Any]) -> dict[str, Any]:
    """Schema 1 had no scaled concurrency runs."""
    return {"work_scale": 1, **record}


def derive_summary(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in map(with_work_scale, map(without_false_durability, records)):
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
            # without a flush in the phase, device counters show whatever writeback landed, not this phase
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


def comparable_rate(row: dict[str, Any]) -> tuple[Any, str]:
    """Use the operation rate for phases without a durable rate."""
    durable = row.get("durable_throughput_mib_s_median")
    if durable is not None:
        return durable, "durable"
    return row.get("throughput_mib_s_median"), "operation"


def derive_scaling(summary: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_key: dict[tuple[str, str, str], dict[tuple[int, int], dict[str, Any]]] = (
        defaultdict(dict)
    )
    for row in summary:
        if not str(row["scenario"]).startswith("concurrency-"):
            continue
        if row.get("worker_id") is not None:
            continue
        key = (str(row["filesystem"]), str(row["scenario"]), str(row["phase"]))
        by_key[key][(int(row["worker_count"]), int(row["work_scale"]))] = row
    output: list[dict[str, Any]] = []
    for key, shapes in sorted(by_key.items()):
        if (1, 1) not in shapes or (4, 1) not in shapes:
            continue
        one, one_basis = comparable_rate(shapes[(1, 1)])
        four, four_basis = comparable_rate(shapes[(4, 1)])
        if not one or four is None or one_basis != four_basis:
            continue
        same_total = (
            comparable_rate(shapes[(1, 4)])[0] if (1, 4) in shapes else None
        )
        output.append(
            {
                "filesystem": key[0],
                "scenario": key[1],
                "phase": key[2],
                "one_worker_mib_s": one,
                "one_worker_same_total_mib_s": same_total,
                "four_worker_mib_s": four,
                "scaling_ratio": float(four) / float(one),
                # Split scaling_ratio into dataset-size and worker-count effects.
                "size_effect_ratio": (
                    None if same_total is None else float(same_total) / float(one)
                ),
                "worker_effect_ratio": (
                    float(four) / float(same_total) if same_total else None
                ),
                "rate_basis": one_basis,
            }
        )
    return output


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Aggregate BSFS JSONL results and render an HTML report"
    )
    parser.add_argument("run_dir", type=Path)
    return parser.parse_args()


def analyze_run(run_dir: Path) -> Path:
    """Writes derived/ for a finished run and returns the report path."""
    raw = run_dir / "raw" / "results.jsonl"
    if not raw.exists():
        raise FileNotFoundError(f"results not found: {raw}")
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
        record_count=len(records),
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
    return output / "report.html"


def main() -> int:
    args = parse_args()
    try:
        analyze_run(args.run_dir.resolve())
    except FileNotFoundError as error:
        raise SystemExit(str(error)) from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
