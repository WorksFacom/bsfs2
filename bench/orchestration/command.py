from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
from typing import Iterable, Mapping


@dataclass
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str


class CommandError(RuntimeError):
    def __init__(self, result: CommandResult):
        self.result = result
        rendered = " ".join(result.argv)
        super().__init__(f"command failed ({result.returncode}): {rendered}")


class CommandRunner:
    def __init__(self, run_dir: Path, dry_run: bool = False):
        self.run_dir = run_dir
        self.dry_run = dry_run
        self.command_log = run_dir / "commands.jsonl"
        self._counter = 0
        if not dry_run:
            run_dir.mkdir(parents=True, exist_ok=True)

    def _record(
        self,
        argv: tuple[str, ...],
        cwd: Path | None,
        result: CommandResult | None,
        label: str,
    ) -> None:
        record = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "label": label,
            "argv": list(argv),
            "cwd": str(cwd) if cwd else None,
            "dry_run": self.dry_run,
            "returncode": result.returncode if result else None,
        }
        if self.dry_run:
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
    ) -> CommandResult:
        normalized = tuple(os.fspath(item) for item in argv)
        if self.dry_run:
            self._record(normalized, cwd, None, label)
            return CommandResult(normalized, 0, "", "")

        self._counter += 1
        completed = subprocess.run(
            normalized,
            cwd=cwd,
            env=dict(env) if env else None,
            input=input_text,
            text=True,
            capture_output=True,
            check=False,
        )
        result = CommandResult(
            normalized, completed.returncode, completed.stdout, completed.stderr
        )
        log_path = self.run_dir / "logs"
        log_path.mkdir(exist_ok=True)
        safe_label = "".join(
            char if char.isalnum() or char in "-_" else "_" for char in label
        )
        stem = f"{self._counter:05d}-{safe_label}"
        (log_path / f"{stem}.stdout").write_text(
            completed.stdout, encoding="utf-8"
        )
        (log_path / f"{stem}.stderr").write_text(
            completed.stderr, encoding="utf-8"
        )
        self._record(normalized, cwd, result, label)
        if check and completed.returncode != 0:
            raise CommandError(result)
        return result

    def write_control(self, path: Path, value: str, label: str) -> None:
        if self.dry_run:
            self._record(("write", str(path), value), None, None, label)
            return
        path.write_text(value, encoding="ascii")
        self._record(("write", str(path), value), None, None, label)

