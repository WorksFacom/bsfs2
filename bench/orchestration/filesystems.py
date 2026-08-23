from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import shutil
from typing import Any, Iterable, Mapping

from .command import CommandRunner
from .modules import (
    DEFAULT_STRATEGY,
    CustomBsfsArtifacts,
    alloc_strategies,
    module_path,
)

REPOSITORY = Path(__file__).resolve().parents[2]


class FilesystemUnavailable(RuntimeError):
    pass


@dataclass
class FilesystemContext:
    runner: CommandRunner
    repository: Path
    device: str
    stable_device: str
    mountpoint: Path
    run_id: str
    # carried here so cache.py can report through adapter.context without every
    # adapter method growing an argument
    reporter: Any | None = None


class FilesystemAdapter:
    name = ""
    mounted = True
    required_tools: tuple[str, ...] = ()
    mount_options = ""

    def __init__(self, context: FilesystemContext):
        self.context = context

    def check_available(self) -> None:
        missing = [tool for tool in self.required_tools if shutil.which(tool) is None]
        if missing:
            raise FilesystemUnavailable(
                f"{self.name}: missing tools: {', '.join(missing)}"
            )

    def format_argv(self) -> list[str]:
        raise NotImplementedError

    def format(self) -> None:
        self.context.runner.run(self.format_argv(), label=f"{self.name}-format")

    def mount(self) -> None:
        raise NotImplementedError

    def unmount(self) -> None:
        if not self.mounted:
            return
        self.context.runner.run(
            ["umount", str(self.context.mountpoint)],
            check=False,
            label=f"{self.name}-unmount",
        )
        self.assert_unmounted()

    def assert_unmounted(self) -> None:
        """A silently-failed umount would send wipefs at a live filesystem."""
        if self.context.runner.dry_run:
            return
        target = str(self.context.mountpoint)
        for line in Path("/proc/self/mountinfo").read_text(
            encoding="utf-8"
        ).splitlines():
            fields = line.split()
            if len(fields) > 4 and fields[4].replace("\\040", " ") == target:
                raise RuntimeError(
                    f"{self.name}: {target} is still mounted after umount"
                )

    def validate_health(self) -> None:
        return

    def version(self) -> str:
        return "unknown"

    def _tool_version(self, argv: list[str]) -> str:
        result = self.context.runner.run(
            argv, check=False, label=f"{self.name}-version"
        )
        output = result.stdout.strip() or result.stderr.strip()
        return output.splitlines()[0] if output else "unknown"

    def configuration(self) -> dict[str, str]:
        """Describes what this adapter actually does, so the manifest cannot
        drift from the commands it records."""
        return {
            "format_command": " ".join(self.format_argv()),
            "mount_options": self.mount_options,
        }

    @property
    def target_argument(self) -> tuple[str, str]:
        return "--root", str(self.context.mountpoint)


class BsfsAdapter(FilesystemAdapter):
    name = "bsfs"
    required_tools = ("mount", "umount", "insmod", "rmmod")
    mount_options = "noatime"
    # The source file that selected this allocator. Every strategy registers
    # the same "bsfs" type. Only the loaded .ko differs.
    alloc_strategy: str | None = DEFAULT_STRATEGY

    @property
    def module(self) -> Path:
        return module_path(self.context.repository, self.alloc_strategy)

    def format_argv(self) -> list[str]:
        return [str(self.context.repository / "mkfs.bsfs"), self.context.device]

    def version(self) -> str:
        described = self.context.runner.run(
            ["git", "-C", str(self.context.repository), "describe",
             "--always", "--dirty"],
            check=False,
            label=f"{self.name}-version",
        )
        described_text = described.stdout.strip() or "unknown"
        return f"{described_text} alloc={self.alloc_strategy}"

    def configuration(self) -> dict[str, str]:
        return {
            **super().configuration(),
            "alloc_strategy": self.alloc_strategy,
            "module": str(self.module),
        }

    def mount(self) -> None:
        self.context.runner.run(
            ["rmmod", "bsfs2"],
            check=False,
            label=f"{self.name}-remove-stale-module",
        )
        self.context.runner.run(
            ["insmod", str(self.module)],
            label=f"{self.name}-load-module",
        )
        if not self.context.runner.dry_run:
            self.context.mountpoint.mkdir(parents=True, exist_ok=True)
        self.context.runner.run(
            [
                "mount",
                "-t",
                "bsfs",
                "-o",
                self.mount_options,
                self.context.device,
                str(self.context.mountpoint),
            ],
            label=f"{self.name}-mount",
        )

    def unmount(self) -> None:
        super().unmount()
        self.context.runner.run(
            ["rmmod", "bsfs2"], check=False, label=f"{self.name}-unload-module"
        )


class CustomBsfsAdapter(BsfsAdapter):
    """BSFS implementation supplied as an already-built artifact pair."""

    # an out-of-tree build is not one of this checkout's allocators
    alloc_strategy = None

    def __init__(
        self, context: FilesystemContext, artifacts: CustomBsfsArtifacts
    ) -> None:
        super().__init__(context)
        self.name = artifacts.name
        self.artifacts = artifacts

    @property
    def module(self) -> Path:
        return self.artifacts.module

    def format_argv(self) -> list[str]:
        return [str(self.artifacts.formatter), self.context.device]

    def version(self) -> str:
        return (
            f"custom module={self.artifacts.module_sha256[:16]} "
            f"mkfs={self.artifacts.formatter_sha256[:16]}"
        )

    def configuration(self) -> dict[str, str]:
        return {
            **FilesystemAdapter.configuration(self),
            "artifact_kind": "custom-bsfs",
            "module": str(self.artifacts.module),
            "formatter": str(self.artifacts.formatter),
            "source_module": str(self.artifacts.source_module),
            "source_formatter": str(self.artifacts.source_formatter),
            "module_sha256": self.artifacts.module_sha256,
            "formatter_sha256": self.artifacts.formatter_sha256,
            "module_name": self.artifacts.module_name,
            "vermagic": self.artifacts.vermagic,
            "correctness_gate_coverage": "publication-validation-only",
        }


class OldBsfsAdapter(FilesystemAdapter):
    name = "old-bsfs"
    mounted = False
    mount_options = "raw-userspace-api"

    def format_argv(self) -> list[str]:
        return [
            str(self.context.repository / "bench/bin/mkfs-old-bsfs"),
            self.context.device,
        ]

    def version(self) -> str:
        described = self.context.runner.run(
            ["git", "-C", str(self.context.repository.parent / "bsfs_old"),
             "describe", "--always", "--dirty"],
            check=False,
            label="old-bsfs-version",
        )
        return described.stdout.strip() or "unknown"

    def mount(self) -> None:
        return

    def unmount(self) -> None:
        return

    @property
    def target_argument(self) -> tuple[str, str]:
        return "--device", self.context.device


class Ext4Adapter(FilesystemAdapter):
    name = "ext4"
    required_tools = ("mkfs.ext4", "mount", "umount")
    mount_options = "noatime,nodiscard"

    def format_argv(self) -> list[str]:
        return [
            "mkfs.ext4",
            "-F",
            "-b",
            "4096",
            "-E",
            "lazy_itable_init=0,lazy_journal_init=0",
            self.context.device,
        ]

    def version(self) -> str:
        return self._tool_version(["mkfs.ext4", "-V"])

    def mount(self) -> None:
        if not self.context.runner.dry_run:
            self.context.mountpoint.mkdir(parents=True, exist_ok=True)
        self.context.runner.run(
            [
                "mount",
                "-t",
                "ext4",
                "-o",
                self.mount_options,
                self.context.device,
                str(self.context.mountpoint),
            ],
            label="ext4-mount",
        )


class BtrfsAdapter(FilesystemAdapter):
    name = "btrfs"
    required_tools = ("mkfs.btrfs", "mount", "umount")
    mount_options = "noatime,nodiscard"

    def format_argv(self) -> list[str]:
        # -m single is explicit: mkfs.btrfs defaults to DUP metadata on a single
        # device, which doubles every metadata write and would show up as btrfs
        # write amplification for reasons unrelated to its design
        return [
            "mkfs.btrfs",
            "-f",
            "-s",
            "4096",
            "-m",
            "single",
            "-d",
            "single",
            self.context.device,
        ]

    def version(self) -> str:
        return self._tool_version(["mkfs.btrfs", "--version"])

    def mount(self) -> None:
        if not self.context.runner.dry_run:
            self.context.mountpoint.mkdir(parents=True, exist_ok=True)
        self.context.runner.run(
            [
                "mount",
                "-t",
                "btrfs",
                "-o",
                self.mount_options,
                self.context.device,
                str(self.context.mountpoint),
            ],
            label="btrfs-mount",
        )


class ZfsAdapter(FilesystemAdapter):
    name = "zfs"
    required_tools = ("zpool", "zfs")
    # stated rather than inherited: at the 128K default a 4K random overwrite
    # is a 128K read-modify-write, which is ZFS's real behaviour but is not
    # comparable to the 4K-block filesystems without saying so
    recordsize = "128K"
    checksum = "on"

    @property
    def mount_options(self) -> str:
        return (
            f"ashift=12,recordsize={self.recordsize},atime=off,"
            f"compression=off,dedup=off,checksum={self.checksum},"
            "sync=standard,cachefile=none"
        )

    def __init__(self, context: FilesystemContext):
        super().__init__(context)
        safe = "".join(char if char.isalnum() else "_" for char in context.run_id)
        self.pool = f"bsfsbench_{safe}"[:48]

    def format_argv(self) -> list[str]:
        return [
            "zpool",
            "create",
            "-f",
            "-o",
            "ashift=12",
            "-o",
            "cachefile=none",
            "-O",
            "atime=off",
            "-O",
            "compression=off",
            "-O",
            "dedup=off",
            "-O",
            f"checksum={self.checksum}",
            "-O",
            f"recordsize={self.recordsize}",
            "-O",
            "sync=standard",
            "-O",
            f"mountpoint={self.context.mountpoint}",
            self.pool,
            self.context.stable_device,
        ]

    def version(self) -> str:
        return self._tool_version(["zfs", "--version"])

    def format(self) -> None:
        if not self.context.runner.dry_run:
            self.context.mountpoint.mkdir(parents=True, exist_ok=True)
        self.context.runner.run(
            self.format_argv(), label="zfs-create-pool"
        )

    def mount(self) -> None:
        # zpool create/import mounts the root dataset automatically.
        return

    def unmount(self) -> None:
        self.context.runner.run(
            ["zpool", "export", self.pool],
            check=False,
            label="zfs-export-pool",
        )
        self.assert_unmounted()

    def import_pool(self) -> None:
        self.context.runner.run(
            [
                "zpool",
                "import",
                "-d",
                str(Path(self.context.stable_device).parent),
                self.pool,
            ],
            label="zfs-import-pool",
        )

    def validate_health(self) -> None:
        if self.context.runner.dry_run:
            self.context.runner.run(
                ["zpool", "status", "-x", self.pool],
                label="zfs-health-check",
            )
            return
        result = self.context.runner.run(
            ["zpool", "status", "-x", self.pool],
            label="zfs-health-check",
        )
        if "is healthy" not in result.stdout:
            raise RuntimeError(f"ZFS pool {self.pool} is not healthy")


class Zfs4KAdapter(ZfsAdapter):
    """ZFS matched to the 4K block size the other filesystems are formatted
    with, so random-overwrite results are comparable as well as realistic."""

    name = "zfs-4k"
    recordsize = "4K"


ADAPTERS: dict[str, type[FilesystemAdapter]] = {
    "bsfs": BsfsAdapter,
    "old-bsfs": OldBsfsAdapter,
    "ext4": Ext4Adapter,
    "btrfs": BtrfsAdapter,
    "zfs": ZfsAdapter,
    "zfs-4k": Zfs4KAdapter,
}

# Generate one target per allocation strategy. Adding src/alloc_<name>.c makes
# it benchmarkable without an orchestrator change. Plain "bsfs" remains the
# default-strategy target.
for _strategy in alloc_strategies(REPOSITORY):
    ADAPTERS[f"bsfs-{_strategy}"] = type(
        f"Bsfs{_strategy.capitalize()}Adapter",
        (BsfsAdapter,),
        {"name": f"bsfs-{_strategy}", "alloc_strategy": _strategy},
    )

# Code that handles BSFS targets specially must use this membership list, not a
# "bsfs" prefix test. old-bsfs is a separate filesystem.
BSFS_FILESYSTEMS = ("bsfs", *(f"bsfs-{s}" for s in alloc_strategies(REPOSITORY)))


def bsfs_targets_by_strategy(
    filesystems: Iterable[str],
) -> dict[str, tuple[str, ...]]:
    """Return required builds in first-use order.

    Each value lists the targets that share its module. "bsfs" and
    "bsfs-linear" use the same module.
    """
    grouped: dict[str, tuple[str, ...]] = {}
    for name in filesystems:
        if name not in BSFS_FILESYSTEMS:
            continue
        strategy = ADAPTERS[name].alloc_strategy  # type: ignore[attr-defined]
        grouped[strategy] = (*grouped.get(strategy, ()), name)
    return grouped


def create_adapter(
    name: str,
    context: FilesystemContext,
    custom_bsfs: Mapping[str, CustomBsfsArtifacts] | None = None,
) -> FilesystemAdapter:
    if custom_bsfs is not None and name in custom_bsfs:
        if name in ADAPTERS:
            raise ValueError(f"custom filesystem shadows a built-in target: {name}")
        adapter = CustomBsfsAdapter(context, custom_bsfs[name])
        adapter.check_available()
        return adapter
    try:
        adapter = ADAPTERS[name](context)
    except KeyError as error:
        raise ValueError(f"unknown filesystem: {name}") from error
    adapter.check_available()
    return adapter
