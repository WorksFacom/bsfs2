from __future__ import annotations

from pathlib import Path
from typing import Any, Iterable

from .command import CommandError, CommandRunner


GATES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("smoke", ("tests/smoke.sh", "tests/smoke_guest.sh")),
    ("persistence", ("tests/smoke.sh", "tests/persist_guest.sh")),
    ("copy-on-write", ("tests/smoke.sh", "tests/cow_guest.sh")),
    ("crash", ("tests/crash.sh",)),
    ("enospc-crash", ("tests/crash.sh", "enospc_crash")),
    ("fuzz", ("tests/smoke.sh", "tests/fuzz_guest.sh")),
    ("alias", ("tests/smoke.sh", "tests/alias_guest.sh")),
    ("reclaim", ("tests/smoke.sh", "tests/reclaim_guest.sh")),
)


class BenchmarkBinaryError(RuntimeError):
    pass


def validate_benchmark_binaries(
    repository: Path, filesystems: Iterable[str]
) -> None:
    """Require the C runners needed by the selected filesystem adapters."""
    selected = set(filesystems)
    required: list[Path] = []
    if selected - {"old-bsfs"}:
        required.append(repository / "bench/bin/bench-posix")
    if "old-bsfs" in selected:
        required.extend(
            (
                repository / "bench/bin/bench-old",
                repository / "bench/bin/mkfs-old-bsfs",
            )
        )
    missing = [
        path
        for path in required
        if not path.is_file() or not path.stat().st_mode & 0o111
    ]
    if not missing:
        return

    command = (
        "make -C bench OLD_BSFS_DIR=/absolute/path/to/bsfs_old"
        if "old-bsfs" in selected
        else "make -C bench bin/bench-posix"
    )
    paths = ", ".join(str(path.relative_to(repository)) for path in missing)
    raise BenchmarkBinaryError(
        f"benchmark C binaries are missing or not executable: {paths}; "
        f"build them first with: {command}"
    )


class CorrectnessGateError(RuntimeError):
    """Carries the outcomes collected so far so the failing gate still reaches
    the manifest instead of being discarded with the exception."""

    def __init__(self, outcomes: list[dict[str, Any]], cause: CommandError):
        self.outcomes = outcomes
        self.cause = cause
        failed = outcomes[-1]["name"] if outcomes else "unknown"
        super().__init__(f"correctness gate failed: {failed}")


def run_correctness_gates(
    runner: CommandRunner, repository: Path
) -> list[dict[str, Any]]:
    outcomes: list[dict[str, Any]] = []
    for name, argv in GATES:
        command = [str(repository / argv[0]), *argv[1:]]
        try:
            result = runner.run(
                command, cwd=repository, label=f"correctness-{name}"
            )
        except CommandError as error:
            outcomes.append(
                {
                    "name": name,
                    "passed": False,
                    "returncode": error.result.returncode,
                }
            )
            raise CorrectnessGateError(outcomes, error) from error
        outcomes.append(
            {"name": name, "passed": True, "returncode": result.returncode}
        )
    return outcomes
