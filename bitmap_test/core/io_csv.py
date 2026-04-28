"""
CSV output helpers for benchmark artifacts.

`write_csv`:
- creates the parent directory if it does not exist.
- accepts `header_labels` to rename columns in the CSV without changing internal key names
  (e.g. `ops_per_sec` → `ops_per_sec[ops/s]`).
- enforces deterministic column order via the explicit `fieldnames` list.
"""

from __future__ import annotations

import csv
from pathlib import Path


def write_csv(path, rows, fieldnames, header_labels=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    header_labels = header_labels or {}
    csv_headers = [header_labels.get(key, key) for key in fieldnames]

    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=csv_headers)
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    header_labels.get(key, key): row.get(key)
                    for key in fieldnames
                }
            )
    return path.resolve()
