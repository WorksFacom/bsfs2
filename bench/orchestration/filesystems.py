from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import shutil
from typing import Any

from .command import CommandRunner


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

    def format_argv(self) -> list[str]:
        return [str(self.context.repository / "mkfs.bsfs"), self.context.device]

    def version(self) -> str:
        described = self.context.runner.run(
            ["git", "-C", str(self.context.repository), "describe",
             "--always", "--dirty"],
            check=False,
            label="bsfs-version",
        )
        return described.stdout.strip() or "unknown"

    def mount(self) -> None:
        self.context.runner.run(
            ["rmmod", "bsfs2"], check=False, label="bsfs-remove-stale-module"
        )
        self.context.runner.run(
            ["insmod", str(self.context.repository / "bsfs2.ko")],
            label="bsfs-load-module",
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
            label="bsfs-mount",
        )

    def unmount(self) -> None:
        super().unmount()
        self.context.runner.run(
            ["rmmod", "bsfs2"], check=False, label="bsfs-unload-module"
        )


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


ADAPTERS = {
    "bsfs": BsfsAdapter,
    "old-bsfs": OldBsfsAdapter,
    "ext4": Ext4Adapter,
    "btrfs": BtrfsAdapter,
    "zfs": ZfsAdapter,
    "zfs-4k": Zfs4KAdapter,
}


def create_adapter(name: str, context: FilesystemContext) -> FilesystemAdapter:
    try:
        adapter = ADAPTERS[name](context)
    except KeyError as error:
        raise ValueError(f"unknown filesystem: {name}") from error
    adapter.check_available()
    return adapter
