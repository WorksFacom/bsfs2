from __future__ import annotations

from html import escape
import json
from pathlib import Path
import re
from typing import Any


REPORT_METRICS = (
    (
        "durable_throughput_mib_s",
        "Durable throughput",
        "MiB/s",
        "Higher is better",
        "Logical data divided by operation time plus any recorded final flush; higher is better.",
    ),
    (
        "durable_iops",
        "Durable IOPS",
        "ops/s",
        "Higher is better",
        "Completed operations divided by operation time plus any recorded final flush; higher is better.",
    ),
    (
        "durable_total_ns",
        "Durable total time",
        "ns",
        "Lower is better",
        "Operation-path time plus any recorded final flush; lower is better.",
    ),
    (
        "latency_p99_ns",
        "p99 latency",
        "ns",
        "Lower is better",
        "Nearest-rank p99 of individual operation latencies, excluding the final flush; lower is better.",
    ),
    (
        "write_amplification",
        "Write amplification",
        "device bytes / logical bytes",
        "Lower is better",
        "Device bytes written per logical byte, shown only for phases with a final flush; lower is better.",
    ),
)


PHASE_DESCRIPTIONS = {
    "bulk": {
        "sequential-write": (
            "Creates one file, writes it sequentially in 1 MiB requests, then flushes it."
        ),
        "sequential-read-cold": (
            "Reads and validates the whole file sequentially after caches are dropped."
        ),
        "sequential-read-warm": (
            "Repeats the sequential read and validation with the file data cached."
        ),
        "random-read-4k": (
            "Performs deterministic 4 KiB random reads and validates every block."
        ),
        "random-overwrite-4k": (
            "Performs deterministic 4 KiB in-place overwrites, then flushes them."
        ),
        "verify-overwrites": (
            "Rereads every overwritten block and validates its most recent contents."
        ),
    },
    "metadata": {
        "mkdir": "Creates the benchmark directory tree, then flushes it.",
        "create": "Creates and closes empty files across the directory tree, then flushes it.",
        "lookup-stat-cold": (
            "Stats every file in randomized order after caches are dropped and validates its size."
        ),
        "lookup-stat-warm": (
            "Repeats the randomized stat pass with dentries cached and validates each file's size."
        ),
        "iteration": (
            "Enumerates every directory and verifies that every expected file appears exactly once."
        ),
        "rename": (
            "Renames a fixed subset of files into neighboring directories, then flushes the changes."
        ),
        "native-namespace-removal": (
            "Unlinks every file from the namespace, then flushes the removals."
        ),
        "directory-removal": (
            "Removes the emptied child directories and benchmark root, then flushes the removals."
        ),
        "verify-empty-namespace": (
            "Checks that the removed benchmark namespace no longer exists."
        ),
    },
    "mixed": {
        "create-tree": (
            "Creates and writes a tree of mixed-size files, then flushes the complete tree."
        ),
        "cold-read-tree": (
            "Reads the mixed-size files in randomized order after caches are dropped and validates their contents."
        ),
        "append-4k": "Appends 4 KiB to a selected fifth of the files.",
        "overwrite-4k": "Overwrites 4 KiB in place in a different selected fifth of the files.",
        "rename": "Renames a selected tenth of the files within the tree.",
        "native-namespace-removal": "Unlinks a selected fifth of the files from the namespace.",
        "edit-cycle-durable-total": (
            "Combines the append, overwrite, rename, and unlink edit phases and their final flush."
        ),
        "verify-tree": (
            "Checks every file's expected path, size, contents, and removal state after the edit cycle."
        ),
    },
    "concurrency-seq": {
        "sequential-write": (
            "Concurrent workers each write a separate file sequentially in 1 MiB requests."
        ),
        "sequential-read-cold": (
            "Concurrent workers each read and validate a separate file after caches are dropped."
        ),
    },
    "concurrency-mixed": {
        "mixed-create": (
            "Concurrent workers each create and write a separate mixed-size file tree."
        ),
        "mixed-read": (
            "Concurrent workers each read and validate their separate mixed-size "
            "file tree after caches are dropped."
        ),
        "mixed-edit": (
            "Concurrent workers append, overwrite, rename, or unlink selected files in their own trees."
        ),
        "mixed-verify": (
            "Concurrent workers validate every expected path, size, content change, and removal in their own trees."
        ),
    },
    "recovery-pressure": {
        "seed-create": (
            "Creates and writes the initial set of 64 KiB recovery-pressure files, then flushes them."
        ),
        "seed-native-namespace-removal": (
            "Unlinks the initial seed files, records reclaim activity, then flushes the removals."
        ),
        "verify-survivors": (
            "Enumerates the final refill's surviving files and validates every file's contents."
        ),
    },
    "old-recovery": {
        "create": (
            "Creates and writes the 64 KiB files used by the old-BSFS recovery "
            "test, then flushes them."
        ),
        "native-retention-logical-deletion": (
            "Logically deletes the files while old-BSFS retains recovery entries, then flushes the deletions."
        ),
        "retention-verify": (
            "Lists retained recovery entries and verifies that every deleted file is represented."
        ),
        "restore-fixed-subset": (
            "Restores a fixed subset of retained files, validates their contents, then flushes the restores."
        ),
        "explicit-purge": (
            "Explicitly purges retained recovery data, records reclaimed blocks, then flushes the purge."
        ),
    },
}


def _phase_description(scenario: str, phase: str) -> str:
    """describe a workload phase, including the generated per-worker names."""
    base_phase = phase
    worker_suffix = ""
    worker_match = re.fullmatch(r"(.+)-worker-(\d+)", phase)
    if scenario in {"concurrency-seq", "concurrency-mixed"} and worker_match:
        base_phase = worker_match.group(1)
        worker_suffix = (
            f" This chart isolates worker {worker_match.group(2)} within each "
            "worker-count configuration."
        )
    elif scenario in {"concurrency-seq", "concurrency-mixed"}:
        worker_suffix = (
            " This chart aggregates all workers in each worker-count configuration."
        )

    description = PHASE_DESCRIPTIONS.get(scenario, {}).get(base_phase)
    if description is None and scenario == "recovery-pressure":
        refill_match = re.fullmatch(r"cycle-(\d+)-refill", phase)
        removal_match = re.fullmatch(r"cycle-(\d+)-native-namespace-removal", phase)
        if refill_match:
            description = (
                f"Recovery cycle {refill_match.group(1)} creates 64 KiB files "
                "until the reclaim target or ENOSPC is reached, then flushes them."
            )
        elif removal_match:
            description = (
                f"Recovery cycle {removal_match.group(1)} unlinks the preceding "
                "refill files, records reclaim activity, then flushes the removals."
            )
    if description is None:
        description = f'Compares filesystems for benchmark phase "{phase}".'
    return description + worker_suffix


def _phase_descriptions(
    summary: list[dict[str, Any]],
) -> dict[str, dict[str, str]]:
    descriptions: dict[str, dict[str, str]] = {}
    for row in summary:
        scenario = row.get("scenario")
        phase = row.get("phase")
        if scenario is None or phase is None:
            continue
        scenario_name = str(scenario)
        phase_name = str(phase)
        descriptions.setdefault(scenario_name, {})[phase_name] = _phase_description(
            scenario_name, phase_name
        )
    return descriptions


def _json_for_html(value: Any) -> str:
    """Serialize data without allowing it to terminate the script element.

    allow_nan is off because NaN/Infinity aren't valid JSON. one slipping
    through would make JSON.parse throw and blank the whole report, and
    failing loud here beats shipping an empty page.
    """
    return (
        json.dumps(
            value, ensure_ascii=False, separators=(",", ":"), allow_nan=False
        )
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
        .replace("&", "\\u0026")
    )


def _failed_by_phase(
    failures: list[dict[str, Any]],
) -> dict[str, dict[str, list[str]]]:
    """a filesystem missing from a chart because it failed shouldn't read as
    "not run". failures with no phase apply to every phase in the scenario,
    so they land under the empty key."""
    collected: dict[str, dict[str, set[str]]] = {}
    for failure in failures:
        scenario = failure.get("scenario")
        filesystem = failure.get("filesystem")
        if not scenario or not filesystem:
            continue
        phase = failure.get("phase")
        collected.setdefault(str(scenario), {}).setdefault(
            str(phase) if phase else "", set()
        ).add(str(filesystem))
    return {
        scenario: {phase: sorted(names) for phase, names in sorted(phases.items())}
        for scenario, phases in sorted(collected.items())
    }


def _manifest_context(manifest: dict[str, Any]) -> tuple[str, str, str]:
    config = manifest.get("config")
    if not isinstance(config, dict):
        config = {}
    kernel = manifest.get("kernel")
    if not isinstance(kernel, dict):
        kernel = {}

    items = (
        ("Profile", config.get("profile", "unknown")),
        ("Host", manifest.get("hostname", "unknown")),
        ("Kernel", kernel.get("release", "unknown")),
        ("Started", manifest.get("created_at", "unknown")),
        ("Completed", manifest.get("completed_at", "unknown")),
        ("Repetitions", config.get("repetitions", "unknown")),
    )
    context = "".join(
        f"<dt>{escape(str(label))}</dt><dd>{escape(str(value))}</dd>"
        for label, value in items
    )

    schedule_items = []
    schedule = manifest.get("filesystem_schedule")
    if isinstance(schedule, list):
        for entry in schedule:
            if not isinstance(entry, dict):
                continue
            order = entry.get("order")
            if not isinstance(order, list):
                continue
            repetition = escape(str(entry.get("repetition", "?")))
            filesystems = " &rarr; ".join(escape(str(item)) for item in order)
            schedule_items.append(
                f"<li><span>Repetition {repetition}</span>{filesystems}</li>"
            )
    schedule_html = (
        "<ul class=\"schedule\">" + "".join(schedule_items) + "</ul>"
        if schedule_items
        else "<p class=\"empty\">No filesystem schedule recorded.</p>"
    )

    overrides = []
    if manifest.get("shared_disk_override_used"):
        overrides.append("shared-disk override")
    if manifest.get("size_range_override_used"):
        overrides.append("size-range override")
    override_text = ", ".join(overrides) if overrides else "No recorded overrides"
    return context, schedule_html, override_text


def render_report(
    *,
    run_dir: Path,
    record_count: int,
    summary: list[dict[str, Any]],
    scaling: list[dict[str, Any]],
    failures: list[dict[str, Any]],
    skipped: list[str],
    manifest: dict[str, Any],
) -> str:
    """Render one complete, dependency-free benchmark report."""
    fields = sorted({field for row in summary for field in row})
    filesystems = sorted(
        {str(row.get("filesystem")) for row in summary if row.get("filesystem")}
    )
    scenarios = sorted(
        {str(row.get("scenario")) for row in summary if row.get("scenario")}
    )
    payload = _json_for_html(
        {
            "summary": summary,
            "scaling": scaling,
            "failures": failures,
            "failedByPhase": _failed_by_phase(failures),
            "skipped": skipped,
            "fields": fields,
            "metrics": REPORT_METRICS,
            "phaseDescriptions": _phase_descriptions(summary),
        }
    )

    run_id = str(manifest.get("run_id") or run_dir.name)
    eligibility = manifest.get("publication_eligible")
    if eligibility is True:
        status = "Publication eligible"
        status_class = "badge good"
    elif eligibility is False:
        status = "Not publication eligible"
        status_class = "badge warning"
    else:
        status = "Eligibility not recorded"
        status_class = "badge"
    context, schedule, overrides = _manifest_context(manifest)

    replacements = {
        "__TITLE__": escape(f"BSFS benchmark report — {run_id}"),
        "__RUN_ID__": escape(run_id),
        "__STATUS__": escape(status),
        "__STATUS_CLASS__": status_class,
        "__RECORD_COUNT__": str(record_count),
        "__GROUP_COUNT__": str(len(summary)),
        "__FILESYSTEM_COUNT__": str(len(filesystems)),
        "__SCENARIO_COUNT__": str(len(scenarios)),
        "__FAILURE_COUNT__": str(len(failures)),
        "__WARNING_COUNT__": str(len(skipped)),
        "__CONTEXT__": context,
        "__SCHEDULE__": schedule,
        "__OVERRIDES__": escape(overrides),
        "__REPORT_DATA__": payload,
    }
    # single pass so a substituted value never gets rescanned for a later
    # marker; longest first so no marker gets shadowed by a prefix of itself
    pattern = "|".join(
        re.escape(marker)
        for marker in sorted(replacements, key=len, reverse=True)
    )
    return re.sub(
        pattern, lambda match: replacements[match.group()], REPORT_TEMPLATE
    )


def write_report(path: Path, **kwargs: Any) -> None:
    path.write_text(render_report(**kwargs), encoding="utf-8")


REPORT_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
:root {
  color-scheme: light;
  --page: #f4f7f8;
  --panel: #ffffff;
  --panel-raised: #edf3f6;
  --line: #c8d5db;
  --ink: #14232d;
  --muted: #5d707b;
  --accent: #087f72;
  --blue: #276dcc;
  --warning: #8c5a08;
  --danger: #b4232f;
  --shadow: 0 18px 50px rgb(31 55 67 / 12%);
  --glow-blue: #d8edf7;
  --glow-green: #d8eee9;
  --panel-alpha: rgb(255 255 255 / 92%);
  --control: #ffffff;
  --notice-warning: #fff3d6;
  --notice-danger: #fde9ec;
  --axis: #6b8190;
  --range-cap: #516874;
  --table-head: #dce9ee;
  --table-hover: #e7f1f5;
}
html[data-theme="dark"] {
  color-scheme: dark;
  --page: #071018;
  --panel: #0e1a25;
  --panel-raised: #132432;
  --line: #294153;
  --ink: #edf5f8;
  --muted: #9db0bd;
  --accent: #5eead4;
  --blue: #72a7ff;
  --warning: #ffc768;
  --danger: #ff8585;
  --shadow: 0 18px 50px rgb(0 0 0 / 22%);
  --glow-blue: #1d4966;
  --glow-green: #153f3d;
  --panel-alpha: rgb(14 26 37 / 92%);
  --control: #09141d;
  --notice-warning: #322818;
  --notice-danger: #341c22;
  --axis: #6b8190;
  --range-cap: #a9bbc5;
  --table-head: #182c3a;
  --table-hover: #173040;
}
* { box-sizing: border-box; }
html { scroll-behavior: smooth; }
body {
  margin: 0;
  background:
    radial-gradient(circle at 92% -5%, var(--glow-blue) 0, transparent 30rem),
    radial-gradient(circle at -10% 22%, var(--glow-green) 0, transparent 26rem),
    var(--page);
  color: var(--ink);
  font: 15px/1.5 Inter, ui-sans-serif, system-ui, sans-serif;
}
main { width: min(1480px, 100%); margin: auto; padding: 34px 24px 72px; }
h1, h2, h3, p { margin-top: 0; }
h1 { margin-bottom: 7px; font-size: clamp(2.2rem, 5vw, 4.6rem); letter-spacing: -.055em; line-height: 1; }
h2 { margin-bottom: 4px; font-size: 1.35rem; letter-spacing: -.015em; }
h3 { margin-bottom: 10px; font-size: 1rem; }
.eyebrow { color: var(--accent); font-size: .74rem; font-weight: 800; letter-spacing: .14em; text-transform: uppercase; }
.muted, .empty { color: var(--muted); }
.hero { display: flex; align-items: flex-start; justify-content: space-between; gap: 24px; padding-bottom: 28px; border-bottom: 1px solid var(--line); }
.hero-actions { display: flex; flex-direction: column; align-items: flex-end; gap: 10px; }
.badge { display: inline-block; padding: 7px 11px; border: 1px solid var(--line); border-radius: 999px; color: var(--muted); font-size: .8rem; font-weight: 800; white-space: nowrap; }
.badge.good { border-color: var(--accent); color: var(--accent); }
.badge.warning { border-color: var(--warning); color: var(--warning); }
.theme-switch { display: inline-flex; align-items: center; gap: 8px; padding: 6px 10px; border: 1px solid var(--line); border-radius: 999px; background: var(--panel); color: var(--ink); font: inherit; font-size: .8rem; font-weight: 700; cursor: pointer; }
.theme-switch:focus-visible { outline: 3px solid color-mix(in srgb, var(--accent) 45%, transparent); outline-offset: 2px; }
.switch-track { position: relative; width: 32px; height: 18px; border-radius: 999px; background: var(--line); transition: background .15s ease; }
.switch-knob { position: absolute; top: 3px; left: 3px; width: 12px; height: 12px; border-radius: 50%; background: var(--panel); box-shadow: 0 1px 3px rgb(0 0 0 / 30%); transition: transform .15s ease; }
.theme-switch[aria-checked="true"] .switch-track { background: var(--accent); }
.theme-switch[aria-checked="true"] .switch-knob { transform: translateX(14px); }
section { margin-top: 36px; }
.stat-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(155px, 1fr)); gap: 12px; margin-top: 18px; }
.stat, .panel, details { border: 1px solid var(--line); border-radius: 13px; background: var(--panel-alpha); box-shadow: var(--shadow); }
.stat { padding: 16px; }
.stat span { display: block; color: var(--muted); font-size: .78rem; font-weight: 700; text-transform: uppercase; letter-spacing: .06em; }
.stat strong { display: block; margin-top: 4px; color: var(--accent); font-size: 1.8rem; line-height: 1.1; }
.context-grid { display: grid; grid-template-columns: minmax(280px, 1fr) minmax(280px, 1fr); gap: 12px; margin-top: 15px; }
.panel { padding: 17px; min-width: 0; }
.provenance { display: grid; grid-template-columns: auto minmax(0, 1fr); gap: 5px 18px; margin: 0; }
dt { color: var(--muted); }
dd { margin: 0; overflow-wrap: anywhere; }
.schedule { margin: 0; padding: 0; list-style: none; }
.schedule li { display: grid; grid-template-columns: 8rem 1fr; gap: 12px; padding: 6px 0; border-bottom: 1px solid var(--line); }
.schedule li:last-child { border: 0; }
.schedule span { color: var(--muted); }
.notice { margin: 12px 0; padding: 14px; border-left: 3px solid var(--warning); border-radius: 5px; background: var(--notice-warning); }
.notice.failure { border-color: var(--danger); background: var(--notice-danger); }
details { margin-top: 12px; overflow: clip; }
summary { padding: 15px 17px; cursor: pointer; font-weight: 800; }
details[open] > summary { border-bottom: 1px solid var(--line); }
.details-body { padding: 16px; }
.scenario > summary { font-size: 1.05rem; }
.phase-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 420px), 1fr)); align-items: start; gap: 12px; }
.phase { margin: 0; background: var(--panel-raised); box-shadow: none; }
select, input {
  width: 100%; padding: 9px 10px; border: 1px solid var(--line); border-radius: 8px;
  background: var(--control); color: var(--ink); font: inherit;
}
.chart { min-height: 300px; margin-top: 12px; overflow-x: auto; }
.chart svg { display: block; width: 100%; height: 300px; }
.axis, .range { stroke: var(--axis); stroke-width: 1; }
.range-cap { stroke: var(--range-cap); stroke-width: 2; }
.bar { fill: var(--blue); transition: fill .15s ease; }
.bar:hover { fill: var(--accent); }
.chart-description { margin: 9px 0 0; color: var(--muted); font-size: .86rem; line-height: 1.45; }
.chart-note { margin: 10px 0 0; padding: 8px 10px; border-left: 3px solid var(--danger); border-radius: 4px; background: var(--notice-danger); font-size: .85rem; }
.chart-label { fill: var(--muted); font-size: 11px; }
.chart-value { fill: var(--ink); font-size: 11px; font-weight: 700; }
.table-wrap { max-height: 72vh; margin-top: 12px; overflow: auto; border: 1px solid var(--line); border-radius: 9px; }
table { width: max-content; min-width: 100%; border-collapse: collapse; }
th, td { padding: 8px 10px; border-bottom: 1px solid var(--line); text-align: left; white-space: nowrap; }
th { position: sticky; top: 0; z-index: 1; background: var(--table-head); color: var(--accent); font-size: .76rem; }
td { font-variant-numeric: tabular-nums; }
tbody tr:hover td { background: var(--table-hover); }
th button { margin: -8px -10px; padding: 8px 10px; border: 0; background: transparent; color: inherit; font: inherit; font-weight: 800; cursor: pointer; }
.table-tools { display: grid; grid-template-columns: minmax(220px, 1fr) auto; align-items: center; gap: 12px; }
.row-count { color: var(--muted); white-space: nowrap; }
noscript { display: block; margin-top: 20px; padding: 14px; border: 1px solid var(--danger); border-radius: 8px; }
@media (max-width: 720px) {
  main { padding: 24px 14px 56px; }
  .hero { display: block; }
  .hero-actions { align-items: flex-start; margin-top: 12px; }
  .context-grid { grid-template-columns: 1fr; }
  .table-tools { grid-template-columns: 1fr; }
}
@media print {
  :root, html[data-theme="dark"] { color-scheme: light; --page: white; --panel: white; --panel-raised: white; --panel-alpha: white; --ink: #111; --muted: #555; --line: #bbb; --control: white; --table-head: #eee; --table-hover: white; --notice-warning: white; --notice-danger: white; }
  body { background: white; }
  .stat, .panel, details { box-shadow: none; }
  .table-wrap { max-height: none; }
  .theme-switch { display: none; }
}
</style>
</head>
<body>
<main>
  <header class="hero">
    <div>
      <div class="eyebrow">BSFS benchmark analysis</div>
      <h1>__RUN_ID__</h1>
      <p class="muted">A self-contained view of the run’s derived results.</p>
    </div>
    <div class="hero-actions">
      <div class="__STATUS_CLASS__">__STATUS__</div>
      <button id="theme-switch" class="theme-switch" type="button" role="switch" aria-checked="false" aria-label="Switch to dark mode">
        <span class="switch-track" aria-hidden="true"><span class="switch-knob"></span></span>
        <span id="theme-label">Light mode</span>
      </button>
    </div>
  </header>

  <section aria-labelledby="overview-title">
    <h2 id="overview-title">Overview</h2>
    <div class="stat-grid">
      <div class="stat"><span>Raw records</span><strong>__RECORD_COUNT__</strong></div>
      <div class="stat"><span>Summary groups</span><strong>__GROUP_COUNT__</strong></div>
      <div class="stat"><span>Filesystems</span><strong>__FILESYSTEM_COUNT__</strong></div>
      <div class="stat"><span>Scenarios</span><strong>__SCENARIO_COUNT__</strong></div>
      <div class="stat"><span>Failed phases</span><strong>__FAILURE_COUNT__</strong></div>
      <div class="stat"><span>Input warnings</span><strong>__WARNING_COUNT__</strong></div>
    </div>
  </section>

  <section aria-labelledby="context-title">
    <h2 id="context-title">Run context</h2>
    <div class="context-grid">
      <div class="panel"><h3>Provenance</h3><dl class="provenance">__CONTEXT__<dt>Overrides</dt><dd>__OVERRIDES__</dd></dl></div>
      <div class="panel"><h3>Filesystem schedule</h3>__SCHEDULE__</div>
    </div>
  </section>

  <section id="health" aria-label="Run health"></section>

  <section aria-labelledby="comparisons-title">
    <h2 id="comparisons-title">Metric comparisons</h2>
    <p class="muted">Bars show medians and whiskers show the observed minimum–maximum range. Durable measurements include the final flush. No combined filesystem score is calculated.</p>
    <div id="scenario-groups"></div>
  </section>

  <section aria-labelledby="scaling-title">
    <h2 id="scaling-title">Concurrency scaling</h2>
    <p class="muted">Aggregate durable throughput at one and four workers.</p>
    <div id="scaling"></div>
  </section>

  <section aria-labelledby="complete-title">
    <details id="complete-data">
      <summary id="complete-title">Complete derived data</summary>
      <div class="details-body">
        <p class="muted">Every field written to summary.csv is available here.</p>
        <div class="table-tools">
          <input id="summary-filter" type="search" placeholder="Filter filesystem, scenario, phase, or value" aria-label="Filter complete derived data">
          <span id="summary-count" class="row-count"></span>
        </div>
        <div id="summary-table"></div>
      </div>
    </details>
  </section>

  <noscript>This report requires JavaScript to render its embedded charts and tables. It does not make any network requests.</noscript>
</main>

<script id="report-data" type="application/json">__REPORT_DATA__</script>
<script>
(() => {
  "use strict";
  const data = JSON.parse(document.querySelector("#report-data").textContent);
  const svgNamespace = "http://www.w3.org/2000/svg";
  const completeSort = { field: null, ascending: true };
  const themeSwitch = document.querySelector("#theme-switch");
  const themeLabel = document.querySelector("#theme-label");

  function setTheme(theme) {
    const dark = theme === "dark";
    document.documentElement.dataset.theme = dark ? "dark" : "light";
    themeSwitch.setAttribute("aria-checked", String(dark));
    themeSwitch.setAttribute("aria-label", `Switch to ${dark ? "light" : "dark"} mode`);
    themeLabel.textContent = dark ? "Dark mode" : "Light mode";
  }

  themeSwitch.addEventListener("click", () => {
    setTheme(document.documentElement.dataset.theme === "dark" ? "light" : "dark");
  });
  setTheme("light");

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function svgElement(tag, attributes = {}, text) {
    const node = document.createElementNS(svgNamespace, tag);
    Object.entries(attributes).forEach(([name, value]) => node.setAttribute(name, String(value)));
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function formatValue(value) {
    if (value === null || value === undefined || value === "") return "—";
    if (typeof value !== "number") return String(value);
    // pinned to en-US so a decimal comma doesn't get misread as a thousands separator
    const magnitude = Math.abs(value);
    if (magnitude >= 1000) return value.toLocaleString("en-US", { maximumFractionDigits: 1 });
    return value.toLocaleString("en-US", { maximumFractionDigits: 3 });
  }

  function compareValues(left, right) {
    if (left === right) return 0;
    if (left === null || left === undefined) return 1;
    if (right === null || right === undefined) return -1;
    if (typeof left === "number" && typeof right === "number") return left - right;
    return String(left).localeCompare(String(right), undefined, { numeric: true });
  }

  function makeTable(rows, fields, sortable = false) {
    if (!rows.length) return element("p", "empty", "No data recorded.");
    const wrap = element("div", "table-wrap");
    const table = element("table");
    const thead = element("thead");
    const header = element("tr");
    fields.forEach((field) => {
      const th = element("th");
      if (sortable) {
        const suffix = completeSort.field === field ? (completeSort.ascending ? " ▲" : " ▼") : "";
        const button = element("button", "", field + suffix);
        button.type = "button";
        button.addEventListener("click", () => {
          if (completeSort.field === field) completeSort.ascending = !completeSort.ascending;
          else { completeSort.field = field; completeSort.ascending = true; }
          renderCompleteTable();
        });
        th.append(button);
      } else {
        th.textContent = field;
      }
      header.append(th);
    });
    thead.append(header);
    const tbody = element("tbody");
    rows.forEach((row) => {
      const tr = element("tr");
      fields.forEach((field) => tr.append(element("td", "", formatValue(row[field]))));
      tbody.append(tr);
    });
    table.append(thead, tbody);
    wrap.append(table);
    return wrap;
  }

  function renderHealth() {
    const health = document.querySelector("#health");
    if (data.skipped.length) {
      const warning = element("div", "notice");
      warning.append(
        element("h3", "", `Unreadable result lines (${data.skipped.length})`),
        makeTable(data.skipped.map((message) => ({ message })), ["message"]),
      );
      health.append(warning);
    }
    if (data.failures.length) {
      const failure = element("div", "notice failure");
      const fields = [...new Set(data.failures.flatMap((row) => Object.keys(row)))].sort();
      failure.append(
        element("h3", "", `Retained failed phases (${data.failures.length})`),
        makeTable(data.failures, fields),
      );
      health.append(failure);
    }
    if (!data.skipped.length && !data.failures.length) {
      health.append(element("p", "notice", "All parsed benchmark phases passed validation."));
    }
  }

  function workerLabel(row) {
    if (row.worker_id === null || row.worker_id === undefined) return "aggregate";
    return `worker ${row.worker_id}`;
  }

  function renderChart(rows, metric) {
    const [name, title, unit, direction] = metric;
    const medianField = `${name}_median`;
    const minimumField = `${name}_minimum`;
    const maximumField = `${name}_maximum`;
    const values = rows
      .filter((row) => Number.isFinite(row[medianField]))
      .sort((left, right) =>
        String(left.filesystem).localeCompare(String(right.filesystem)) ||
        Number(left.worker_count) - Number(right.worker_count) ||
        (left.worker_id ?? -1) - (right.worker_id ?? -1));
    const container = element("div", "chart");
    if (!values.length) {
      container.append(element("p", "empty", "No measurement for this metric."));
      return container;
    }

    const width = Math.max(560, values.length * 94 + 65);
    const height = 300;
    const top = 40;
    const baseline = 238;
    // scale to the data, not to 1, so sub-1 metrics don't render as slivers
    const observed = Math.max(
      ...values.map((row) => Number.isFinite(row[maximumField]) ? row[maximumField] : row[medianField]),
    );
    const maximum = observed > 0 ? observed : 1;
    const y = (value) => baseline - (value / maximum) * (baseline - top);
    const svg = svgElement("svg", { viewBox: `0 0 ${width} ${height}`, role: "img", "aria-label": `${title}, ${direction}` });
    if (values.length > 5) svg.style.minWidth = `${width}px`;
    svg.append(
      svgElement("line", { x1: 48, x2: width - 12, y1: baseline, y2: baseline, class: "axis" }),
      svgElement("text", { x: 48, y: 17, class: "chart-label" }, `${title} (${unit}) · ${direction}`),
      svgElement("text", { x: 8, y: baseline + 4, class: "chart-label" }, "0"),
      svgElement("text", { x: 8, y: top + 4, class: "chart-label" }, formatValue(maximum)),
    );

    values.forEach((row, index) => {
      const x = 58 + index * 94;
      const median = row[medianField];
      const minimum = Number.isFinite(row[minimumField]) ? row[minimumField] : median;
      const observedMaximum = Number.isFinite(row[maximumField]) ? row[maximumField] : median;
      const center = x + 28;
      const medianY = y(median);
      const minimumY = y(minimum);
      const maximumY = y(observedMaximum);
      const tooltip = `${row.filesystem} · workers=${row.worker_count} · ${workerLabel(row)}\nmedian ${formatValue(median)} ${unit}\nrange ${formatValue(minimum)}–${formatValue(observedMaximum)} ${unit}`;
      const bar = svgElement("rect", {
        x, y: medianY, width: 56, height: Math.max(0, baseline - medianY), rx: 4, class: "bar",
      });
      bar.append(svgElement("title", {}, tooltip));
      svg.append(
        svgElement("line", { x1: center, x2: center, y1: maximumY, y2: minimumY, class: "range" }),
        svgElement("line", { x1: center - 6, x2: center + 6, y1: maximumY, y2: maximumY, class: "range-cap" }),
        svgElement("line", { x1: center - 6, x2: center + 6, y1: minimumY, y2: minimumY, class: "range-cap" }),
        bar,
        svgElement("text", { x, y: Math.max(31, medianY - 7), class: "chart-value" }, formatValue(median)),
        svgElement("text", { x, y: 257, class: "chart-label" }, `${row.filesystem} · w${row.worker_count}`),
        svgElement("text", { x, y: 274, class: "chart-label" }, workerLabel(row)),
      );
    });
    container.append(svg);
    return container;
  }

  function failedFor(scenario, phase) {
    const phases = data.failedByPhase[scenario];
    if (!phases) return [];
    return [...new Set([...(phases[phase] || []), ...(phases[""] || [])])].sort();
  }

  function descriptionFor(scenario, phase) {
    return data.phaseDescriptions[scenario]?.[phase] ||
      `Compares filesystems for benchmark phase "${phase}".`;
  }

  function renderComparisons() {
    const grouped = new Map();
    data.summary.forEach((row) => {
      if (!grouped.has(row.scenario)) grouped.set(row.scenario, new Map());
      const phases = grouped.get(row.scenario);
      if (!phases.has(row.phase)) phases.set(row.phase, []);
      phases.get(row.phase).push(row);
    });
    const byName = (left, right) => String(left[0]).localeCompare(String(right[0]));
    const target = document.querySelector("#scenario-groups");
    [...grouped.entries()].sort(byName).forEach(([scenario, phases], scenarioIndex) => {
      const scenarioDetails = element("details", "scenario");
      scenarioDetails.open = scenarioIndex === 0;
      scenarioDetails.append(element("summary", "", `${scenario} · ${phases.size} phases`));
      const scenarioBody = element("div", "details-body phase-grid");
      [...phases.entries()].sort(byName).forEach(([phase, rows], phaseIndex) => {
        const phaseDetails = element("details", "phase");
        phaseDetails.open = scenarioIndex === 0 && phaseIndex === 0;
        phaseDetails.append(element("summary", "", phase));
        const phaseBody = element("div", "details-body");
        const available = data.metrics.filter((metric) =>
          rows.some((row) => Number.isFinite(row[`${metric[0]}_median`])),
        );
        if (!available.length) {
          phaseBody.append(element("p", "empty", "No chartable metrics for this phase."));
        } else {
          const selector = element("select");
          available.forEach((metric, index) => {
            const option = element("option", "", metric[1]);
            option.value = String(index);
            selector.append(option);
          });
          const chartHolder = element("div");
          const description = element("p", "chart-description");
          const draw = () => {
            const metric = available[Number(selector.value)];
            chartHolder.replaceChildren(renderChart(rows, metric));
            description.textContent = `${descriptionFor(scenario, phase)} ${metric[4]}`;
          };
          selector.addEventListener("change", draw);
          phaseBody.append(selector, chartHolder, description);
          draw();
        }
        const failed = failedFor(scenario, phase);
        if (failed.length) {
          phaseBody.append(element("p", "chart-note", `Failed here, absent from the chart: ${failed.join(", ")}`));
        }
        phaseDetails.append(phaseBody);
        scenarioBody.append(phaseDetails);
      });
      scenarioDetails.append(scenarioBody);
      target.append(scenarioDetails);
    });
    if (!grouped.size) target.append(element("p", "empty", "No passed measurements to chart."));
  }

  function renderScaling() {
    document.querySelector("#scaling").append(makeTable(
      data.scaling,
      ["filesystem", "scenario", "phase", "one_worker_mib_s", "four_worker_mib_s", "scaling_ratio"],
    ));
  }

  function renderCompleteTable() {
    const term = document.querySelector("#summary-filter").value.toLocaleLowerCase();
    const rows = data.summary.filter((row) =>
      !term || data.fields.some((field) => String(row[field] ?? "").toLocaleLowerCase().includes(term)),
    );
    if (completeSort.field) {
      rows.sort((left, right) => {
        const order = compareValues(left[completeSort.field], right[completeSort.field]);
        return completeSort.ascending ? order : -order;
      });
    }
    document.querySelector("#summary-count").textContent = `${rows.length} of ${data.summary.length} rows`;
    document.querySelector("#summary-table").replaceChildren(makeTable(rows, data.fields, true));
  }

  document.querySelector("#summary-filter").addEventListener("input", renderCompleteTable);
  renderHealth();
  renderComparisons();
  renderScaling();
  renderCompleteTable();
})();
</script>
</body>
</html>
"""
