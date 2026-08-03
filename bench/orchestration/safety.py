from __future__ import annotations

from dataclasses import asdict, dataclass
import os
from pathlib import Path
import re
import stat
from typing import Iterable


class SafetyError(RuntimeError):
    pass


@dataclass(frozen=True)
class DeviceInfo:
    requested: str
    resolved: str
    name: str
    parent: str
    major: int
    minor: int
    size_bytes: int
    logical_sector_size: int
    physical_sector_size: int
    model: str
    serial: str
    firmware: str
    scheduler: str
    discard_max_bytes: int

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


def _read_text(path: Path, default: str = "") -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (FileNotFoundError, PermissionError, OSError):
        return default


def _read_int(path: Path, default: int = 0) -> int:
    try:
        return int(_read_text(path))
    except ValueError:
        return default


def _sysfs_for_devnum(major: int, minor: int) -> Path:
    path = Path(f"/sys/dev/block/{major}:{minor}")
    if not path.exists():
        raise SafetyError(f"no sysfs entry for block device {major}:{minor}")
    return path.resolve()


def _parent_name(sysfs: Path) -> str:
    if (sysfs / "partition").exists():
        return sysfs.parent.name
    return sysfs.name


def _devnum(path: Path) -> tuple[int, int]:
    status = path.stat()
    if not stat.S_ISBLK(status.st_mode):
        raise SafetyError(f"target is not a block device: {path}")
    return os.major(status.st_rdev), os.minor(status.st_rdev)


def _device_ancestors(major: int, minor: int) -> set[tuple[int, int]]:
    pending = [(major, minor)]
    found: set[tuple[int, int]] = set()
    while pending:
        current = pending.pop()
        if current in found:
            continue
        found.add(current)
        sysfs = _sysfs_for_devnum(*current)
        if (sysfs / "partition").exists():
            dev_text = _read_text(sysfs.parent / "dev")
            if ":" in dev_text:
                pending.append(tuple(int(value) for value in dev_text.split(":")))
        slaves = sysfs / "slaves"
        if slaves.exists():
            for slave in slaves.iterdir():
                dev_text = _read_text(slave / "dev")
                if ":" in dev_text:
                    pending.append(
                        tuple(int(value) for value in dev_text.split(":"))
                    )
    return found


def _mount_devices() -> list[tuple[tuple[int, int], str]]:
    devices: list[tuple[tuple[int, int], str]] = []
    for line in Path("/proc/self/mountinfo").read_text(
        encoding="utf-8"
    ).splitlines():
        fields = line.split()
        major, minor = (int(value) for value in fields[2].split(":"))
        mountpoint = fields[4].replace("\\040", " ")
        devices.append(((major, minor), mountpoint))
    return devices


def _swap_devices() -> set[tuple[int, int]]:
    devices: set[tuple[int, int]] = set()
    lines = Path("/proc/swaps").read_text(encoding="utf-8").splitlines()[1:]
    for line in lines:
        source = Path(line.split()[0])
        try:
            devices.add(_devnum(source.resolve()))
        except (FileNotFoundError, SafetyError):
            continue
    return devices


def _holders(sysfs: Path) -> tuple[str, ...]:
    holder_dir = sysfs / "holders"
    if not holder_dir.exists():
        return ()
    return tuple(sorted(entry.name for entry in holder_dir.iterdir()))


def inspect_physical_device(
    requested: str,
    confirmation: str | None,
    *,
    repository: Path,
    min_size: int,
    max_size: int,
    allow_size_override: bool,
    allow_shared_disk: bool = False,
) -> DeviceInfo:
    if not requested.startswith("/dev/disk/by-id/"):
        raise SafetyError("physical targets must use /dev/disk/by-id/")
    if not re.search(r"-part\d+$", requested):
        raise SafetyError("physical target must be a partition by-id path")
    requested_path = Path(requested)
    try:
        resolved_path = requested_path.resolve(strict=True)
    except FileNotFoundError as error:
        raise SafetyError(f"target does not exist: {requested}") from error
    major, minor = _devnum(resolved_path)
    if confirmation != str(resolved_path):
        raise SafetyError(
            "--confirm must exactly match the resolved partition path "
            f"({resolved_path})"
        )
    sysfs = _sysfs_for_devnum(major, minor)
    if not (sysfs / "partition").exists():
        raise SafetyError("whole-disk targets are rejected")
    parent_name = _parent_name(sysfs)
    parent_sysfs = sysfs.parent
    parent = f"/dev/{parent_name}"
    size_bytes = _read_int(sysfs / "size") * 512
    if not allow_size_override and not (min_size <= size_bytes <= max_size):
        raise SafetyError(
            f"device size {size_bytes} is outside [{min_size}, {max_size}]"
        )

    target_ancestry = _device_ancestors(major, minor)
    repository = repository.resolve()
    # Any mount sharing ancestry with the target is a rejection, not just the
    # ones on a list: a curated list cannot know where this host keeps /efi,
    # /var or anything else, and the cost of being wrong is a wiped partition.
    for devnum, mountpoint in _mount_devices():
        try:
            mounted_ancestry = _device_ancestors(*devnum)
        except SafetyError:
            mounted_ancestry = {devnum}
        if not (target_ancestry & mounted_ancestry):
            continue
        if devnum == (major, minor):
            raise SafetyError(f"target itself is mounted at {mountpoint}")
        try:
            if repository.is_relative_to(Path(mountpoint).resolve()):
                raise SafetyError(
                    f"target shares a disk with the repository mount {mountpoint}"
                )
        except (FileNotFoundError, OSError):
            pass
        if not allow_shared_disk:
            raise SafetyError(
                f"target shares a disk with the mounted filesystem at "
                f"{mountpoint}; pass --allow-shared-disk to override"
            )
    for swap in _swap_devices():
        if target_ancestry & _device_ancestors(*swap):
            raise SafetyError("target or parent backs active swap")
    for candidate in (sysfs, parent_sysfs):
        holders = _holders(candidate)
        if holders:
            raise SafetyError(
                f"{candidate.name} has active holders: {', '.join(holders)}"
            )

    queue = parent_sysfs / "queue"
    device_metadata = parent_sysfs / "device"
    return DeviceInfo(
        requested=requested,
        resolved=str(resolved_path),
        name=sysfs.name,
        parent=parent,
        major=major,
        minor=minor,
        size_bytes=size_bytes,
        logical_sector_size=_read_int(queue / "logical_block_size"),
        physical_sector_size=_read_int(queue / "physical_block_size"),
        model=_read_text(device_metadata / "model"),
        serial=_read_text(device_metadata / "serial"),
        firmware=_read_text(device_metadata / "rev"),
        scheduler=_read_text(queue / "scheduler"),
        discard_max_bytes=_read_int(queue / "discard_max_bytes"),
    )


def validate_loop_image(path: Path, confirmation: str | None) -> None:
    if path.exists():
        if not path.is_file():
            raise SafetyError(f"loop image is not a regular file: {path}")
        if confirmation != str(path.resolve()):
            raise SafetyError(
                "an existing loop image requires --confirm with its resolved path"
            )
    if path.is_symlink():
        raise SafetyError("loop image may not be a symlink")


def cyclic_schedule(items: Iterable[str], repetition: int) -> tuple[str, ...]:
    values = tuple(items)
    if not values:
        return ()
    shift = repetition % len(values)
    return values[shift:] + values[:shift]
