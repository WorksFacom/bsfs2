"""Build and store one BSFS kernel module per allocation strategy.

BSFS_ALLOC in the top-level Makefile selects a src/alloc_<name>.c file. Every
build produces bsfs2.ko, which registers the "bsfs" filesystem type. The
benchmark loads a saved module for the selected strategy.
"""
from __future__ import annotations

from pathlib import Path
import platform
import shutil
import subprocess


DEFAULT_STRATEGY = "linear"

# -DBSFS_ALLOC_STATS adds counters to the allocation fast path. Benchmarks must
# measure the module without those counters. See include/alloc_strategy.h.
MEASURED_STATS = ""


class BsfsModuleError(RuntimeError):
    """The benchmark's BSFS module cannot safely run on this host."""


def alloc_strategies(repository: Path) -> tuple[str, ...]:
    """Return strategies from src/alloc_<name>.c, as the Makefile does."""
    return tuple(
        sorted(
            path.name[len("alloc_") : -len(".c")]
            for path in (repository / "src").glob("alloc_*.c")
        )
    )


def module_path(repository: Path, strategy: str) -> Path:
    """Store a built strategy under bench/build.

    `make -C bench clean` removes this directory, which .gitignore already
    excludes.
    """
    return repository / "bench/build/modules" / f"bsfs2-{strategy}.ko"


def build_module_argv(repository: Path, strategy: str | None = None) -> list[str]:
    """Pass PWD explicitly because make -C leaves it at the caller's directory.

    The kernel build uses M=$(PWD), so this keeps build files in the repository.
    """
    argv = ["make", "-C", str(repository), f"PWD={repository}"]
    if strategy is not None:
        # Command-line assignments override `-include .env`, so its chosen
        # strategy cannot affect a measured build.
        argv.extend([f"BSFS_ALLOC={strategy}", f"BSFS_ALLOC_STATS={MEASURED_STATS}"])
    return argv


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


def _assert_built_as(repository: Path, strategy: str) -> None:
    """Confirm that kbuild recorded the requested strategy.

    .bsfs.o.cmd is the only artifact that records the selected strategy. A
    wrong module is otherwise hard to spot.
    """
    command_file = repository / ".bsfs.o.cmd"
    try:
        recorded = command_file.read_text(encoding="utf-8")
    except OSError as error:
        raise BsfsModuleError(
            f"cannot confirm the {strategy} build: {command_file} is unreadable "
            f"({error})"
        ) from error
    expected = f'-DBSFS_ALLOC_STRATEGY_NAME=\\"{strategy}\\"'
    if expected not in recorded:
        raise BsfsModuleError(
            f"asked for BSFS_ALLOC={strategy} but {command_file} does not record "
            f"{expected}; the module is not the requested build"
        )
    if "-DBSFS_ALLOC_STATS" in recorded:
        raise BsfsModuleError(
            f"the {strategy} build has -DBSFS_ALLOC_STATS; the allocation "
            "counters change the code being measured"
        )


def _probe_load(module: Path) -> None:
    """Check compatibility by loading the module.

    vermagic alone does not prove that the kernel can load a module.
    """
    # An already-loaded module cannot be the module under test. rmmod also
    # fails when no module is loaded, so /sys/module identifies that harmless
    # case.
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
        raise _module_failure("unloading the BSFS preflight module", unload, module)


def build_modules(repository: Path, strategies: tuple[str, ...]) -> tuple[Path, ...]:
    """Build, save, and probe one module per allocation strategy.

    Build every strategy before the matrix starts. That keeps compilers out of
    measured phases and reports a build or load failure before the runner
    touches a device. This code does not use CommandRunner, so a failure cannot
    create a benchmark result directory.
    """
    known = alloc_strategies(repository)
    unknown = tuple(strategy for strategy in strategies if strategy not in known)
    if unknown:
        raise BsfsModuleError(
            f"unknown allocation strategies: {', '.join(unknown)}; "
            f"available: {', '.join(known)}"
        )

    source = repository / "bsfs2.ko"
    stashed: list[Path] = []
    for strategy in strategies:
        build = _run_capture(build_module_argv(repository, strategy))
        if build.returncode != 0:
            raise _module_failure(f"building bsfs2.ko ({strategy})", build, source)
        _assert_built_as(repository, strategy)
        target = module_path(repository, strategy)
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            shutil.copy2(source, target)
        except OSError as error:
            raise BsfsModuleError(
                f"cannot store the {strategy} module: {error}"
            ) from error
        _probe_load(target)
        stashed.append(target)

    # Restore the default build. The correctness gates and QEMU scripts boot
    # $REPO/bsfs2.ko.
    if strategies:
        restore = _run_capture(build_module_argv(repository))
        if restore.returncode != 0:
            raise _module_failure(
                "restoring the default bsfs2.ko build", restore, source
            )
    return tuple(stashed)
