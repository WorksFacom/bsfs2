"""progress and event reporting for a benchmark run.

non-interference contract, because everything here runs inside a measurement
harness:

1. output is emitted only from the main thread and only between
   ``CommandRunner.run`` calls, never while a measured child is alive. the
   parent sits in ``waitpid`` for the whole measured window, so there's no
   window where this module can compete with a pinned worker.
2. no threads, no timers, no signal-driven redraw. a live ticking clock would
   be the only construct able to do I/O during a phase, so there isn't one.
3. stderr only. nothing is ever written under ``run_dir/mnt``, the mountpoint
   of the device under test.
4. metric readback touches ``run_dir/raw/results.jsonl`` on the host
   filesystem, after the phase has exited and before the next one starts.
5. nothing here may raise into the run. a reporting bug degrades output, it
   doesn't abort a three-hour benchmark.
"""
from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import enum
import functools
import json
import os
from pathlib import Path
import sys
import time
from typing import Any, Iterator, TextIO

from .plan import RunPlan


class Level(enum.IntEnum):
    QUIET = 0
    NORMAL = 1
    VERBOSE = 2


class Style:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"


def format_duration(seconds: float) -> str:
    if seconds != seconds or seconds in (float("inf"), float("-inf")):
        return "?"
    if seconds < 60:
        return f"{seconds:.1f}s"
    if seconds < 3600:
        return f"{int(seconds) // 60}m{int(seconds) % 60:02d}s"
    return f"{int(seconds) // 3600}h{(int(seconds) % 3600) // 60:02d}m"


def format_rate(mib_per_second: float) -> str:
    if mib_per_second >= 1024:
        return f"{mib_per_second / 1024:.2f} GiB/s"
    return f"{mib_per_second:.1f} MiB/s"


def format_count(value: float) -> str:
    if value >= 1e6:
        return f"{value / 1e6:.2f}M"
    if value >= 1e3:
        return f"{value / 1e3:.1f}k"
    return f"{value:.0f}"


def format_nanoseconds(value: float) -> str:
    if value >= 1e9:
        return f"{value / 1e9:.2f}s"
    if value >= 1e6:
        return f"{value / 1e6:.2f}ms"
    if value >= 1e3:
        return f"{value / 1e3:.1f}us"
    return f"{value:.0f}ns"


def metric_cells(record: dict[str, Any]) -> tuple[str, str]:
    """the two right-hand columns of a phase line: headline rate, then p99.

    kept separate from ``describe_metrics`` so the reporter can pad each cell
    into its own column; either half may be empty.
    """
    if record.get("validation_status") not in (None, "passed"):
        return f"VALIDATION {record['validation_status']}", ""
    throughput = record.get("durable_throughput_mib_s")
    if not isinstance(throughput, (int, float)):
        throughput = record.get("throughput_mib_s")
    operations = record.get("durable_iops")
    if not isinstance(operations, (int, float)):
        operations = record.get("iops")
    # a metadata phase moves almost no bytes, so its MiB/s is noise
    headline = ""
    if isinstance(throughput, (int, float)) and throughput >= 1.0:
        headline = format_rate(float(throughput))
    elif isinstance(operations, (int, float)) and operations > 0:
        headline = f"{format_count(float(operations))} ops/s"
    p99 = record.get("latency_p99_ns")
    latency = (
        format_nanoseconds(float(p99))
        if isinstance(p99, (int, float)) and p99 > 0
        else ""
    )
    return headline, latency


def describe_metrics(record: dict[str, Any]) -> str:
    """one short headline per phase; display only, never fed back into results."""
    headline, latency = metric_cells(record)
    parts = [headline] if headline else []
    if latency:
        parts.append(f"p99 {latency}")
    return "  ".join(parts) if parts else "ok"


class PhaseMetricsReader:
    """tails the result files, remembering how far each one has been read."""

    def __init__(self) -> None:
        self._offsets: dict[str, int] = {}

    def start(self, path: Path) -> None:
        """anchor at the current end of file so a phase sees only its own rows."""
        key = str(path)
        try:
            self._offsets[key] = path.stat().st_size
        except OSError:
            self._offsets.setdefault(key, 0)

    def collect(self, path: Path) -> dict[str, Any] | None:
        key = str(path)
        offset = self._offsets.get(key, 0)
        try:
            with path.open("rb") as stream:
                stream.seek(offset)
                payload = stream.read()
        except OSError:
            return None
        if not payload:
            return None
        complete, _, remainder = payload.rpartition(b"\n")
        # a partial final line is left for the next read rather than parsed
        self._offsets[key] = offset + len(payload) - len(remainder)
        records: list[dict[str, Any]] = []
        for line in complete.splitlines():
            if not line.strip():
                continue
            try:
                record = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if isinstance(record, dict):
                records.append(record)
        if not records:
            return None
        aggregate = [
            record for record in records if record.get("worker_id") is None
        ]
        return (aggregate or records)[-1]


@dataclass
class FilesystemTally:
    phases: int = 0
    failures: int = 0
    elapsed: float = 0.0


def _guarded(method):
    @functools.wraps(method)
    def wrapper(self, *args, **kwargs):
        try:
            return method(self, *args, **kwargs)
        except Exception:
            self.degraded = True
            return None

    return wrapper


class RunReporter:
    # one grid for every line: step lines span the counter, label and worker
    # cells so their duration lands in the same column as a phase's
    INDENT = 2
    LABEL_WIDTH = 26
    WORKERS_WIDTH = 3
    TIMING_WIDTH = 8
    RATE_WIDTH = 12
    LATENCY_WIDTH = 8

    def __init__(
        self,
        *,
        stream: TextIO | None = None,
        level: Level = Level.NORMAL,
        color: bool | None = None,
    ):
        self.stream = stream if stream is not None else sys.stderr
        self.level = level
        self.degraded = False
        if color is None:
            color = bool(
                getattr(self.stream, "isatty", lambda: False)()
            ) and not os.environ.get("NO_COLOR")
        self.color = color
        self.metrics = PhaseMetricsReader()
        self.plan: RunPlan | None = None
        self.total_phases = 0
        self.completed_phases = 0
        self.failed_phases = 0
        self.filesystem_order: tuple[str, ...] = ()
        self.current_filesystem: str | None = None
        self.tallies: dict[str, FilesystemTally] = {}
        self.started_at: float | None = None
        self._filesystem_started_at: float | None = None
        self._unit_started_at: float | None = None
        self._unit_phase_mark = 0
        self._section_title: str | None = None
        self._section_started_at: float | None = None

    # ------------------------------------------------------------------ io

    def _paint(self, text: str, style: str) -> str:
        return f"{style}{text}{Style.RESET}" if self.color else text

    def _write(self, text: str) -> None:
        self.stream.write(text + "\n")
        self.stream.flush()

    def _line(self, text: str, level: Level = Level.NORMAL) -> None:
        if self.level >= level:
            self._write(text)

    def _tallied(self, filesystem: str | None) -> FilesystemTally:
        return self.tallies.setdefault(filesystem or "-", FilesystemTally())

    # ------------------------------------------------------------- columns

    def _counter_text(self, completed: int) -> str:
        digits = max(len(str(self.total_phases)), 2)
        percent = (
            100 * completed / self.total_phases if self.total_phases else 0
        )
        return f"[{completed:>{digits}}/{self.total_phases} {percent:>3.0f}%]"

    def _counter_width(self) -> int:
        # constant for a run: total_phases is fixed before the first line
        return len(self._counter_text(0))

    def _step_field_width(self) -> int:
        return (
            self._counter_width() + 1 + self.LABEL_WIDTH + 1 + self.WORKERS_WIDTH
        )

    # -------------------------------------------------------------- header

    @_guarded
    def header(
        self,
        *,
        run_id: str,
        config_path: Path,
        profile: str,
        repetitions: int,
        seeds: tuple[int, ...],
        common_cpu: int,
        concurrency_cpus: tuple[int, ...],
        run_dir: Path,
        dry_run: bool,
    ) -> None:
        title = f"bsfs benchmark run {run_id}"
        if dry_run:
            title += " (dry run)"
        self._line(self._paint(title, Style.BOLD), Level.QUIET)
        seed_text = ", ".join(str(seed) for seed in seeds[:repetitions])
        self._line(
            f"  config    {config_path} "
            f"(profile={profile}, repetitions={repetitions}, "
            f"seeds=[{seed_text}])"
        )
        cpu_text = ", ".join(str(cpu) for cpu in concurrency_cpus)
        self._line(f"  cpus      common={common_cpu}, concurrency=[{cpu_text}]")
        self._line(f"  results   {run_dir}")

    @_guarded
    def announce_plan(self, plan: RunPlan) -> None:
        self.plan = plan
        self.total_phases = plan.total_phases
        self.filesystem_order = plan.filesystems
        count = len(plan.filesystems)
        self._line(
            f"  plan      {count} filesystem{'' if count == 1 else 's'}, "
            f"{plan.total_units} mkfs cycles, {plan.total_phases} phases"
        )
        for filesystem in plan.filesystems:
            self._line(
                f"              {filesystem:<14} "
                f"{plan.units_for(filesystem):>3} cycles, "
                f"{plan.phases_for(filesystem):>4} phases",
                Level.VERBOSE,
            )

    @_guarded
    def target(self, description: str) -> None:
        self._line(f"  target    {description}", Level.QUIET)

    @_guarded
    def section(self, title: str) -> None:
        self._close_unit()
        self._close_section()
        self._line("")
        self._line(self._paint(title, Style.BOLD))
        self._section_title = title
        self._section_started_at = time.monotonic()

    @_guarded
    def end_section(self) -> None:
        """closes the open section early, so its total excludes trailing work."""
        self._close_unit()
        self._close_section()

    def _close_section(self) -> None:
        if self._section_title is None or self._section_started_at is None:
            return
        title = self._section_title
        elapsed = time.monotonic() - self._section_started_at
        self._section_title = None
        self._section_started_at = None
        try:
            self._emit_step(f"{title} total", elapsed, None, True, Level.NORMAL)
        except Exception:
            self.degraded = True

    # --------------------------------------------------------------- steps

    def _emit_step(
        self,
        description: str,
        elapsed: float | None,
        detail: str | None,
        ok: bool,
        level: Level,
    ) -> None:
        text = description if detail is None else f"{description} ({detail})"
        line = f"{' ' * self.INDENT}{text.ljust(self._step_field_width())}"
        if elapsed is not None:
            line += " " + format_duration(elapsed).rjust(self.TIMING_WIDTH)
        if not ok:
            line += f"  {self._paint('failed', Style.RED)}"
        self._line(line.rstrip(), level)

    @_guarded
    def step(self, description: str, detail: str | None = None) -> None:
        self._emit_step(description, None, detail, True, Level.NORMAL)

    @contextmanager
    def timed(
        self,
        description: str,
        detail: str | None = None,
        level: Level = Level.NORMAL,
    ) -> Iterator[None]:
        start = time.monotonic()
        ok = True
        try:
            yield
        except BaseException:
            ok = False
            raise
        finally:
            elapsed = time.monotonic() - start
            try:
                self._emit_step(description, elapsed, detail, ok, level)
            except Exception:
                self.degraded = True

    @_guarded
    def command(
        self,
        label: str,
        argv: tuple[str, ...],
        returncode: int,
        elapsed: float,
    ) -> None:
        if self.level < Level.VERBOSE:
            return
        rendered = " ".join(argv)
        if len(rendered) > 110:
            rendered = rendered[:107] + "..."
        marker = "  " if returncode == 0 else self._paint(" !", Style.RED)
        self._line(
            f"{marker}  {self._paint(label, Style.DIM)} "
            f"{format_duration(elapsed)}  {self._paint(rendered, Style.DIM)}",
            Level.VERBOSE,
        )

    @_guarded
    def dry_command(self, argv: tuple[str, ...]) -> None:
        rendered = " ".join(argv).replace("\n", "\\n")
        self._line("+ " + rendered, Level.NORMAL)

    # -------------------------------------------------------------- matrix

    @_guarded
    def unit_start(
        self,
        *,
        matrix: str,
        filesystem: str,
        scenario: str,
        repetition: int,
        workers: int = 1,
    ) -> None:
        self._close_unit()
        if self.started_at is None:
            self.started_at = time.monotonic()
        if filesystem != self.current_filesystem:
            now = time.monotonic()
            if self.current_filesystem and self._filesystem_started_at:
                self._tallied(self.current_filesystem).elapsed += (
                    now - self._filesystem_started_at
                )
            self.current_filesystem = filesystem
            self._filesystem_started_at = now
        position = ""
        if filesystem in self.filesystem_order:
            index = self.filesystem_order.index(filesystem) + 1
            position = f"{index}/{len(self.filesystem_order)} "
        workers_text = f" w{workers}" if workers != 1 else ""
        headline = (
            f"[fs {position}{filesystem}] "
            f"{matrix}/{scenario} r{repetition}{workers_text}"
        )
        self._line("")
        self._line(
            f"{self._paint(headline, Style.CYAN)}{self._eta_suffix()}"
        )
        self._unit_started_at = time.monotonic()
        self._unit_phase_mark = self.completed_phases

    def _close_unit(self) -> None:
        """closed at the next unit start, so the trailing unmount is counted."""
        if self._unit_started_at is None:
            return
        elapsed = time.monotonic() - self._unit_started_at
        phases = self.completed_phases - self._unit_phase_mark
        self._unit_started_at = None
        try:
            detail = f"{phases} phase{'' if phases == 1 else 's'}"
            self._emit_step("unit total", elapsed, detail, True, Level.NORMAL)
        except Exception:
            self.degraded = True

    def _eta_suffix(self) -> str:
        if self.completed_phases < 3 or self.started_at is None:
            return ""
        remaining = self.total_phases - self.completed_phases
        if remaining <= 0:
            return ""
        per_phase = (
            time.monotonic() - self.started_at
        ) / self.completed_phases
        return self._paint(
            f"  eta {format_duration(per_phase * remaining)}", Style.DIM
        )

    @contextmanager
    def phase(
        self,
        *,
        filesystem: str,
        scenario: str,
        phase: str,
        workers: int,
        results: Path | None = None,
    ) -> Iterator[None]:
        if self.started_at is None:
            self.started_at = time.monotonic()
        if results is not None:
            try:
                self.metrics.start(results)
            except Exception:
                self.degraded = True
        start = time.monotonic()
        ok = True
        try:
            yield
        except BaseException:
            ok = False
            raise
        finally:
            elapsed = time.monotonic() - start
            self.completed_phases += 1
            tally = self._tallied(filesystem)
            tally.phases += 1
            if not ok:
                self.failed_phases += 1
            try:
                self._emit_phase(scenario, phase, workers, elapsed, ok, results)
            except Exception:
                self.degraded = True

    def _emit_phase(
        self,
        scenario: str,
        phase: str,
        workers: int,
        elapsed: float,
        ok: bool,
        results: Path | None,
    ) -> None:
        counter = self._counter_text(self.completed_phases)
        line = (
            f"{' ' * self.INDENT}{self._paint(counter, Style.DIM)} "
            f"{f'{scenario}/{phase}'.ljust(self.LABEL_WIDTH)} "
            f"{f'w{workers}'.ljust(self.WORKERS_WIDTH)} "
            f"{format_duration(elapsed).rjust(self.TIMING_WIDTH)}"
        )
        if not ok:
            self._line(f"{line}  {self._paint('FAILED', Style.RED)}")
            return
        record = None if results is None else self.metrics.collect(results)
        if record is not None:
            line += self._metric_columns(record)
        self._line(line.rstrip())

    def _metric_columns(self, record: dict[str, Any]) -> str:
        """padding goes on the plain text, so the escapes never widen a cell."""
        headline, latency = metric_cells(record)
        if headline.startswith("VALIDATION"):
            return "  " + self._paint(headline, Style.YELLOW)
        if not headline and not latency:
            headline = "ok"
        columns = "  " + self._paint(
            headline.rjust(self.RATE_WIDTH), Style.GREEN
        )
        if latency:
            columns += "  " + self._paint(
                f"p99 {latency.rjust(self.LATENCY_WIDTH)}", Style.GREEN
            )
        return columns

    @_guarded
    def cache_event(self, description: str, elapsed: float) -> None:
        self._emit_step(
            f"cache: {description}", elapsed, None, True, Level.NORMAL
        )

    @_guarded
    def failure(
        self,
        filesystem: str,
        scenario: str,
        repetition: int,
        error: Exception,
        log_hint: str | None = None,
    ) -> None:
        self._tallied(filesystem).failures += 1
        message = str(error).replace("\n", " ")
        if len(message) > 160:
            message = message[:157] + "..."
        self._line(
            f"  {self._paint('FAILED', Style.RED)}  "
            f"{filesystem} {scenario} r{repetition}: {message}",
            Level.QUIET,
        )
        if log_hint:
            self._line(f"          see {log_hint}", Level.QUIET)

    @_guarded
    def warning(self, message: str) -> None:
        self._line(
            f"  {self._paint('warning', Style.YELLOW)}  {message}", Level.QUIET
        )

    # ------------------------------------------------------------- closing

    @_guarded
    def summary(
        self,
        *,
        run_id: str,
        failures: list[dict[str, object]],
        kernel_findings: int,
        publication_eligible: bool,
        run_dir: Path,
        interrupted: bool = False,
    ) -> None:
        self._close_unit()
        self._close_section()
        now = time.monotonic()
        if self.current_filesystem and self._filesystem_started_at:
            self._tallied(self.current_filesystem).elapsed += (
                now - self._filesystem_started_at
            )
            self._filesystem_started_at = now
        elapsed = 0.0 if self.started_at is None else now - self.started_at
        state = "interrupted after" if interrupted else "finished in"
        self._line("")
        self._line(
            self._paint(
                f"summary  {run_id}  {state} {format_duration(elapsed)}",
                Style.BOLD,
            ),
            Level.QUIET,
        )
        for filesystem in self.filesystem_order or tuple(self.tallies):
            tally = self.tallies.get(filesystem)
            if tally is None:
                continue
            failed = (
                self._paint(f"{tally.failures} failed", Style.RED)
                if tally.failures
                else "0 failed"
            )
            self._line(
                f"  {filesystem:<14} {tally.phases:>4} phases, {failed}, "
                f"{format_duration(tally.elapsed)}",
                Level.QUIET,
            )
        if self.completed_phases < self.total_phases:
            self._line(
                f"  {self.completed_phases}/{self.total_phases} planned "
                "phases were reached",
                Level.QUIET,
            )
        self._line(
            f"  kernel log: {kernel_findings} relevant findings", Level.QUIET
        )
        self._line(
            f"  publication eligible: {'yes' if publication_eligible else 'no'}",
            Level.QUIET,
        )
        if failures:
            self._line(
                f"  {self._paint(str(len(failures)) + ' failures', Style.RED)}"
                " recorded in manifest.json",
                Level.QUIET,
            )
        if self.degraded:
            self._line(
                "  note: progress reporting hit an internal error; "
                "results are unaffected",
                Level.QUIET,
            )
        self._line(f"  results: {run_dir}", Level.QUIET)
        self._line(
            f"  next:    python3 -m bench.orchestration.analyze {run_dir}",
            Level.QUIET,
        )
