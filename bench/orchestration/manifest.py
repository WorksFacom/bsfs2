from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
from typing import Any, Iterable

BUILT_ARTIFACTS = (
    "bench/bin/bench-posix",
    "bench/bin/bench-old",
    "bench/bin/mkfs-old-bsfs",
    "bsfs2.ko",
    "mkfs.bsfs",
)

from .config import RunConfig
from .safety import DeviceInfo
from .topology import describe as describe_topology


def _read(path: Path, default: str = "") -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (FileNotFoundError, PermissionError, OSError):
        return default


def _capture(argv: Iterable[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            tuple(argv),
            cwd=cwd,
            text=True,
            capture_output=True,
            check=False,
        )
    except FileNotFoundError:
        return "unavailable"
    output = completed.stdout.strip() or completed.stderr.strip()
    return output


def _git(repository: Path) -> dict[str, Any]:
    return {
        "commit": _capture(["git", "rev-parse", "HEAD"], repository),
        "branch": _capture(["git", "branch", "--show-current"], repository),
        "status": _capture(["git", "status", "--porcelain=v1"], repository),
    }


def _artifacts(repository: Path) -> dict[str, Any]:
    """the launcher never rebuilds the C runners, so the recorded commit alone
    can't prove which binaries made these numbers. hash them instead."""
    described: dict[str, Any] = {}
    for relative in BUILT_ARTIFACTS:
        path = repository / relative
        try:
            stat = path.stat()
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError:
            described[relative] = {"present": False}
            continue
        described[relative] = {
            "present": True,
            "size_bytes": stat.st_size,
            "modified_at": datetime.fromtimestamp(
                stat.st_mtime, timezone.utc
            ).isoformat(),
            "sha256": digest,
        }
    return described


def _governors(cpus: tuple[int, ...]) -> dict[str, str]:
    governors: dict[str, str] = {}
    for cpu in cpus:
        link = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
        try:
            policy = link.resolve(strict=True)
        except (FileNotFoundError, OSError):
            continue
        governor = policy / "scaling_governor"
        governors[str(policy)] = _read(governor, "unavailable")
    return governors


def collect_manifest(
    *,
    repository: Path,
    old_repository: Path,
    config_path: Path,
    config: RunConfig,
    run_id: str,
    device: DeviceInfo | dict[str, Any],
    affinity: tuple[int, ...],
    publication_eligible: bool,
) -> dict[str, Any]:
    cpu_model = ""
    for line in _read(Path("/proc/cpuinfo")).splitlines():
        if line.startswith("model name"):
            cpu_model = line.split(":", 1)[1].strip()
            break
    memory = {}
    for line in _read(Path("/proc/meminfo")).splitlines():
        if line.startswith(("MemTotal:", "MemAvailable:")):
            key, value = line.split(":", 1)
            memory[key] = value.strip()
    tools = {}
    commands = {
        "cc": ["cc", "--version"],
        "python": ["python3", "--version"],
        "mkfs.ext4": ["mkfs.ext4", "-V"],
        "mkfs.btrfs": ["mkfs.btrfs", "--version"],
        "zfs": ["zfs", "--version"],
        "zpool": ["zpool", "--version"],
    }
    for name, argv in commands.items():
        captured = _capture(argv)
        tools[name] = captured.splitlines()[0] if captured else ""
    return {
        "schema_version": 1,
        "run_id": run_id,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "publication_eligible": publication_eligible,
        "hostname": socket.gethostname(),
        "kernel": {
            "release": platform.release(),
            "version": platform.version(),
            "command_line": _read(Path("/proc/cmdline")),
        },
        "cpu": {
            "model": cpu_model,
            "online_count": os.cpu_count(),
            "runner_affinity": list(affinity),
            "governors_before_run": _governors(affinity),
            "topology": describe_topology(affinity),
            "lscpu": _capture(["lscpu", "-p=CPU,CORE,SOCKET,NODE"]),
            "interrupts": _read(Path("/proc/interrupts")),
        },
        "memory": memory,
        "device": device.to_dict() if isinstance(device, DeviceInfo) else device,
        "repositories": {
            "current": _git(repository),
            "old": _git(old_repository),
        },
        "built_artifacts": _artifacts(repository),
        "config": {
            "path": str(config_path),
            "profile": config.profile,
            "repetitions": config.repetitions,
            "seeds": list(config.seeds),
            "filesystems": list(config.filesystems),
            "common_scenarios": list(config.common_scenarios),
            "concurrency_scenarios": list(config.concurrency_scenarios),
        },
        "tools": tools,
    }


def write_manifest(run_dir: Path, manifest: dict[str, Any]) -> None:
    run_dir.mkdir(parents=True, exist_ok=True)
    temporary = run_dir / "manifest.json.tmp"
    final = run_dir / "manifest.json"
    temporary.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(final)
