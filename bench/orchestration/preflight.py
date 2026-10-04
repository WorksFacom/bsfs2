from __future__ import annotations

from pathlib import Path


class BenchmarkBinaryError(RuntimeError):
    pass


def validate_benchmark_binaries(repository: Path) -> None:
    """Require the C runner."""
    runner = repository / "bench/bin/bench-posix"
    if runner.is_file() and runner.stat().st_mode & 0o111:
        return
    raise BenchmarkBinaryError(
        "benchmark C runner is missing or not executable: "
        f"{runner.relative_to(repository)}; build it first with: make -C bench"
    )
