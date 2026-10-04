from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import tomllib

from .topology import select_cpus, sibling_conflicts


@dataclass(frozen=True)
class RunConfig:
    repetitions: int
    seeds: tuple[int, ...]
    filesystems: tuple[str, ...]
    common_scenarios: tuple[str, ...]
    concurrency_scenarios: tuple[str, ...]
    common_cpu: int
    concurrency_cpus: tuple[int, ...]
    min_device_gib: int
    max_device_gib: int
    loop_size_mib: int
    discard: bool
    recovery: bool = False
    phase_timeout_s: int = 300


def _resolve_cpus(run: dict) -> tuple[int, tuple[int, ...]]:
    """"auto" picks distinct physical cores off CPU 0. explicit lists still get the SMT check."""
    requested = run["concurrency_cpus"]
    common = run["common_cpu"]
    if requested == "auto" or common == "auto":
        chosen = select_cpus(5)
        return chosen[0], tuple(chosen[1:])
    concurrency = tuple(int(value) for value in requested)
    conflicts = sibling_conflicts(concurrency)
    if conflicts:
        pairs = ", ".join(f"{a}/{b}" for a, b in conflicts)
        raise ValueError(
            f"concurrency_cpus share physical cores ({pairs}); "
            "four workers would not get four cores"
        )
    return int(common), concurrency


def load_config(path: Path) -> RunConfig:
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    run = raw["run"]
    device = raw["device"]
    scenarios = raw["scenarios"]
    common_cpu, concurrency_cpus = _resolve_cpus(run)
    config = RunConfig(
        repetitions=int(run["repetitions"]),
        seeds=tuple(int(value) for value in run["seeds"]),
        filesystems=tuple(str(value) for value in run["filesystems"]),
        common_scenarios=tuple(str(value) for value in scenarios["common"]),
        concurrency_scenarios=tuple(
            str(value) for value in scenarios["concurrency"]
        ),
        common_cpu=common_cpu,
        concurrency_cpus=concurrency_cpus,
        min_device_gib=int(device["min_gib"]),
        max_device_gib=int(device["max_gib"]),
        loop_size_mib=int(device["loop_size_mib"]),
        discard=bool(device["discard"]),
        recovery=bool(run.get("recovery", False)),
        phase_timeout_s=int(run.get("phase_timeout_s", 300)),
    )
    if config.phase_timeout_s <= 0:
        raise ValueError("phase_timeout_s must be positive")
    if config.repetitions <= 0:
        raise ValueError("repetitions must be positive")
    if len(config.seeds) < config.repetitions:
        raise ValueError("one recorded seed is required per repetition")
    if not config.concurrency_cpus:
        raise ValueError("at least one concurrency CPU is required")
    return config

