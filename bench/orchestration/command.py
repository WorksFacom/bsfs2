from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time
from typing import Any, Iterable, Mapping


@dataclass
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool = False


class CommandError(RuntimeError):
    def __init__(self, result: CommandResult):
        self.result = result
        rendered = " ".join(result.argv)
        if result.timed_out:
            super().__init__(f"command timed out: {rendered}")
        else:
            super().__init__(f"command failed ({result.returncode}): {rendered}")


def _text(captured: str | bytes | None) -> str:
    if captured is None:
        return ""
    if isinstance(captured, bytes):
        return captured.decode("utf-8", errors="replace")
    return captured


class CommandRunner:
    def __init__(
        self,
        run_dir: Path,
        dry_run: bool = False,
        reporter: Any | None = None,
        default_timeout: float | None = None,
    ):
        self.run_dir = run_dir
        self.dry_run = dry_run
        self.reporter = reporter
        self.default_timeout = default_timeout
        self.command_log = run_dir / "commands.jsonl"
        # where a failing command's stderr landed, for the progress reporter
        self.last_error_log: Path | None = None
        self._counter = 0
        if not dry_run:
            run_dir.mkdir(parents=True, exist_ok=True)

    def _record(
        self,
        argv: tuple[str, ...],
        cwd: Path | None,
        result: CommandResult | None,
        label: str,
        duration: float | None = None,
    ) -> None:
        record = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "label": label,
            "argv": list(argv),
            "cwd": str(cwd) if cwd else None,
            "dry_run": self.dry_run,
            "returncode": result.returncode if result else None,
            "duration_s": duration,
        }
        if self.dry_run:
            if self.reporter is not None:
                self.reporter.dry_command(argv)
            else:
                print("+", " ".join(argv))
            return
        with self.command_log.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record, sort_keys=True) + "\n")

    def run(
        self,
        argv: Iterable[str | os.PathLike[str]],
        *,
        cwd: Path | None = None,
        check: bool = True,
        label: str = "command",
        env: Mapping[str, str] | None = None,
        input_text: str | None = None,
        timeout: float | None = None,
    ) -> CommandResult:
        normalized = tuple(os.fspath(item) for item in argv)
        if self.dry_run:
            self._record(normalized, cwd, None, label)
            return CommandResult(normalized, 0, "", "")

        self._counter += 1
        started = time.monotonic()
        limit = self.default_timeout if timeout is None else timeout
        try:
            completed = subprocess.run(
                normalized,
                cwd=cwd,
                env=dict(env) if env else None,
                input=input_text,
                text=True,
                capture_output=True,
                check=False,
                timeout=limit,
            )
            result = CommandResult(
                normalized,
                completed.returncode,
                completed.stdout,
                completed.stderr,
            )
        except subprocess.TimeoutExpired as expired:
            # subprocess.run already killed and reaped the child
            result = CommandResult(
                normalized,
                -9,
                _text(expired.stdout),
                _text(expired.stderr) + f"\ntimed out after {limit:g} s\n",
                timed_out=True,
            )
        duration = time.monotonic() - started
        log_path = self.run_dir / "logs"
        log_path.mkdir(exist_ok=True)
        safe_label = "".join(
            char if char.isalnum() or char in "-_" else "_" for char in label
        )
        stem = f"{self._counter:05d}-{safe_label}"
        (log_path / f"{stem}.stdout").write_text(
            result.stdout, encoding="utf-8"
        )
        (log_path / f"{stem}.stderr").write_text(
            result.stderr, encoding="utf-8"
        )
        self.last_error_log = log_path / f"{stem}.stderr"
        self._record(normalized, cwd, result, label, duration)
        if self.reporter is not None:
            self.reporter.command(
                label, normalized, result.returncode, duration
            )
        if result.timed_out or (check and result.returncode != 0):
            raise CommandError(result)
        return result

    def write_control(self, path: Path, value: str, label: str) -> None:
        if self.dry_run:
            self._record(("write", str(path), value), None, None, label)
            return
        path.write_text(value, encoding="ascii")
        self._record(("write", str(path), value), None, None, label)

