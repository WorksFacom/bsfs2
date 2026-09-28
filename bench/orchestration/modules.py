"""Builds and stages the BSFS modules the benchmark loads.

BSFS_ALLOC picks a src/alloc_<name>.c. Every build is bsfs2.ko registering "bsfs", so each strategy's
module is saved separately. Custom targets stage a prebuilt module with its matching formatter.
"""
from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess


DEFAULT_STRATEGY = "linear"

# -DBSFS_ALLOC_STATS puts counters on the allocation fast path, so measured builds leave it empty.
# see include/alloc_strategy.h
MEASURED_STATS = ""

CUSTOM_BSFS_NAME = re.compile(r"bsfs-[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\Z")
DEFAULT_MODULE_REMEDY = (
    "Set KDIR to the running kernel's build tree, rebuild bsfs2.ko, and retry."
)


class BsfsModuleError(RuntimeError):
    """The benchmark's BSFS module cannot safely run on this host."""


@dataclass(frozen=True)
class CustomBsfsArtifacts:
    """An out-of-tree BSFS module and its formatter.

    ``source_*`` is what the caller passed, ``module`` and ``formatter`` are the frozen copies the matrix uses.
    """

    name: str
    source_module: Path
    source_formatter: Path
    module: Path
    formatter: Path
    module_sha256: str
    formatter_sha256: str
    module_name: str
    vermagic: str


def alloc_strategies(repository: Path) -> tuple[str, ...]:
    """Strategies from src/alloc_<name>.c, same discovery as the Makefile."""
    return tuple(
        sorted(
            path.name[len("alloc_") : -len(".c")]
            for path in (repository / "src").glob("alloc_*.c")
        )
    )


def module_path(repository: Path, strategy: str) -> Path:
    """Where a built strategy is saved. gitignored, and `make -C bench clean` wipes it."""
    return repository / "bench/build/modules" / f"bsfs2-{strategy}.ko"


def custom_artifact_paths(repository: Path, name: str) -> tuple[Path, Path]:
    directory = repository / "bench/build/custom" / name
    return directory / "bsfs2.ko", directory / "mkfs.bsfs"


def build_module_argv(repository: Path, strategy: str | None = None) -> list[str]:
    argv = ["make", "-C", str(repository)]
    if strategy is not None:
        # command-line vars beat `-include local.mk`, so its strategy can't leak into a measured build
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


def _module_name(module: Path) -> str:
    """modinfo reads the name from the module's ELF, not from the filename."""
    result = _run_capture(["modinfo", "-F", "name", str(module)])
    if result.returncode != 0:
        output = result.stderr.strip() or result.stdout.strip() or "no output"
        raise BsfsModuleError(f"modinfo could not read {module}: {output}")
    return result.stdout.strip() or "unavailable"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def define_custom_artifacts(
    repository: Path,
    name: str,
    module: Path,
    formatter: Path,
) -> CustomBsfsArtifacts:
    """Validates caller-supplied artifacts without touching the repository."""
    if CUSTOM_BSFS_NAME.fullmatch(name) is None:
        raise BsfsModuleError(
            f"invalid custom BSFS name {name!r}; use a lowercase "
            "'bsfs-<name>' slug containing letters, digits, and hyphens"
        )

    try:
        source_module = module.expanduser().resolve(strict=True)
    except OSError as error:
        raise BsfsModuleError(
            f"custom BSFS {name}: module is unavailable: {module} ({error})"
        ) from error
    try:
        source_formatter = formatter.expanduser().resolve(strict=True)
    except OSError as error:
        raise BsfsModuleError(
            f"custom BSFS {name}: formatter is unavailable: {formatter} ({error})"
        ) from error

    if not source_module.is_file():
        raise BsfsModuleError(
            f"custom BSFS {name}: module is not a regular file: {source_module}"
        )
    if not source_formatter.is_file():
        raise BsfsModuleError(
            f"custom BSFS {name}: formatter is not a regular file: "
            f"{source_formatter}"
        )
    if not os.access(source_formatter, os.X_OK):
        raise BsfsModuleError(
            f"custom BSFS {name}: formatter is not executable: "
            f"{source_formatter}"
        )

    staged_module, staged_formatter = custom_artifact_paths(repository, name)
    if source_module == staged_module.resolve(strict=False):
        raise BsfsModuleError(
            f"custom BSFS {name}: source module must be outside its staging path"
        )
    if source_formatter == staged_formatter.resolve(strict=False):
        raise BsfsModuleError(
            f"custom BSFS {name}: source formatter must be outside its staging path"
        )

    try:
        module_name = _module_name(source_module)
    except BsfsModuleError as error:
        raise BsfsModuleError(f"custom BSFS {name}: {error}") from error
    if module_name != "bsfs2":
        raise BsfsModuleError(
            f"custom BSFS {name}: expected module name 'bsfs2', got "
            f"{module_name!r} from {source_module}"
        )

    try:
        module_digest = _sha256(source_module)
        formatter_digest = _sha256(source_formatter)
    except OSError as error:
        raise BsfsModuleError(
            f"custom BSFS {name}: cannot hash supplied artifacts ({error})"
        ) from error

    return CustomBsfsArtifacts(
        name=name,
        source_module=source_module,
        source_formatter=source_formatter,
        module=staged_module,
        formatter=staged_formatter,
        module_sha256=module_digest,
        formatter_sha256=formatter_digest,
        module_name=module_name,
        vermagic=_module_vermagic(source_module),
    )


def _module_failure(
    step: str,
    result: subprocess.CompletedProcess[str],
    module: Path,
    remedy: str = DEFAULT_MODULE_REMEDY,
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
    """Checks kbuild really built the requested strategy.

    .bsfs.o.cmd is the only place the strategy gets recorded, and a wrong module is otherwise hard to spot.
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


def _filesystem_registered(name: str) -> bool:
    try:
        lines = Path("/proc/filesystems").read_text(encoding="ascii").splitlines()
    except OSError:
        return False
    return any(line.split()[-1:] == [name] for line in lines)


def _probe_load(
    module: Path,
    expected_filesystem: str | None = None,
    remedy: str = DEFAULT_MODULE_REMEDY,
) -> None:
    """Loads and unloads the module, since matching vermagic doesn't prove the kernel will take it."""
    # a loaded bsfs2 can't be the one under test. rmmod also fails when nothing is loaded,
    # so /sys/module tells that harmless case apart
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
        raise _module_failure("loading bsfs2.ko", load, module, remedy)

    registration_error: BsfsModuleError | None = None
    if expected_filesystem and not _filesystem_registered(expected_filesystem):
        registration_error = BsfsModuleError(
            "module loaded but did not register filesystem type "
            f"{expected_filesystem!r}: {module}"
        )

    unload = _run_capture(["rmmod", "bsfs2"])
    if unload.returncode != 0:
        raise _module_failure(
            "unloading the BSFS preflight module", unload, module, remedy
        )
    if registration_error is not None:
        raise registration_error


def stage_custom_artifacts(
    artifacts: CustomBsfsArtifacts,
) -> CustomBsfsArtifacts:
    """Freezes a custom pair, verifies the copy, then probes the staged module."""
    try:
        artifacts.module.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(artifacts.source_module, artifacts.module)
        shutil.copy2(artifacts.source_formatter, artifacts.formatter)
        source_module_digest = _sha256(artifacts.source_module)
        source_formatter_digest = _sha256(artifacts.source_formatter)
        staged_module_digest = _sha256(artifacts.module)
        staged_formatter_digest = _sha256(artifacts.formatter)
    except OSError as error:
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: cannot stage artifacts ({error})"
        ) from error

    if source_module_digest != staged_module_digest:
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: module changed while it was copied"
        )
    if source_formatter_digest != staged_formatter_digest:
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: formatter changed while it was copied"
        )
    # module builds run between define_custom_artifacts and here, so catch a source rebuilt meanwhile
    if artifacts.module_sha256 != staged_module_digest:
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: module changed since it was supplied: "
            f"{artifacts.source_module}"
        )
    if artifacts.formatter_sha256 != staged_formatter_digest:
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: formatter changed since it was "
            f"supplied: {artifacts.source_formatter}"
        )
    if not os.access(artifacts.formatter, os.X_OK):
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: staged formatter is not executable"
        )

    try:
        staged_name = _module_name(artifacts.module)
    except BsfsModuleError as error:
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: {error}"
        ) from error
    if staged_name != "bsfs2":
        raise BsfsModuleError(
            f"custom BSFS {artifacts.name}: staged module name changed to "
            f"{staged_name!r}"
        )
    _probe_load(
        artifacts.module,
        expected_filesystem="bsfs",
        remedy=(
            f"Rebuild the custom {artifacts.name} module against the running "
            "kernel, then pass the rebuilt artifact and retry."
        ),
    )
    return replace(
        artifacts,
        module_sha256=staged_module_digest,
        formatter_sha256=staged_formatter_digest,
        module_name=staged_name,
        vermagic=_module_vermagic(artifacts.module),
    )


def build_modules(repository: Path, strategies: tuple[str, ...]) -> tuple[Path, ...]:
    """Builds, saves, and probes one module per allocation strategy.

    Runs before the matrix, so no compiler runs during measured phases and build or load failures surface
    before any device is touched. Skips CommandRunner so a failure never creates a result directory.
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

    # put the default build back for dev tooling and the optional QEMU gates
    if strategies:
        restore = _run_capture(build_module_argv(repository))
        if restore.returncode != 0:
            raise _module_failure(
                "restoring the default bsfs2.ko build", restore, source
            )
    return tuple(stashed)
