from __future__ import annotations

from pathlib import Path
import platform
import subprocess
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


class BsfsModuleError(RuntimeError):
    """The benchmark's BSFS module cannot safely run on this host."""


def _run_capture(
    argv: list[str], *, cwd: Path | None = None
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            argv,
            cwd=cwd,
            text=True,
            capture_output=True,
            check=False,
        )
    except OSError as error:
        return subprocess.CompletedProcess(argv, 127, "", str(error))


def build_module_argv(repository: Path) -> list[str]:
    """make -C moves CURDIR but leaves PWD pointing at the caller's directory,
    and the kernel build uses M=$(PWD). pass PWD explicitly so the build lands
    in the repo no matter where this got invoked from."""
    return ["make", "-C", str(repository), f"PWD={repository}"]


def _module_loaded() -> bool:
    return Path("/sys/module/bsfs2").exists()


def _module_vermagic(module: Path) -> str:
    result = _run_capture(["modinfo", "-F", "vermagic", str(module)])
    value = result.stdout.strip()
    if value:
        return value
    return "unavailable"


def _module_failure(
    step: str,
    result: subprocess.CompletedProcess[str],
    module: Path,
    remedy: str = (
        "Set KDIR to the running kernel's build tree, rebuild bsfs2.ko, "
        "and retry."
    ),
) -> BsfsModuleError:
    output = (result.stderr.strip() or result.stdout.strip() or "no output")
    return BsfsModuleError(
        f"BSFS module preflight failed while {step} "
        f"(exit {result.returncode}): {' '.join(result.args)}\n"
        f"running kernel: {platform.release()}\n"
        f"module vermagic: {_module_vermagic(module)}\n"
        f"command output: {output}\n"
        f"{remedy}"
    )


def validate_bsfs_module(repository: Path) -> None:
    """Build and probe BSFS before a benchmark can touch its target.

    ``insmod`` is deliberately the compatibility check: vermagic alone cannot
    prove that a module can load on this kernel.  This function does not use a
    CommandRunner, so a failure cannot create a benchmark result directory.
    """
    module = repository / "bsfs2.ko"
    build = _run_capture(build_module_argv(repository))
    if build.returncode != 0:
        raise _module_failure("building bsfs2.ko", build, module)

    # Match the adapter's behavior: an already-loaded stale module cannot be
    # the module being benchmarked. rmmod also fails when nothing's loaded,
    # so /sys/module tells that harmless case apart from one still in use.
    removal = _run_capture(["rmmod", "bsfs2"])
    if removal.returncode != 0 and _module_loaded():
        raise _module_failure(
            "removing the stale bsfs2 module",
            removal,
            module,
            "bsfs2 is loaded and still in use; unmount every bsfs filesystem "
            "and close anything holding one open, then retry.",
        )
    load = _run_capture(["insmod", str(module)])
    if load.returncode != 0:
        raise _module_failure("loading bsfs2.ko", load, module)

    unload = _run_capture(["rmmod", "bsfs2"])
    if unload.returncode != 0:
        raise _module_failure(
            "unloading the BSFS preflight module", unload, module
        )


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
