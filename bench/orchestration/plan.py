"""static description of what a run will execute.

the tables here mirror the explicit control flow in ``run.py``; they exist so a
total can be computed before the first mkfs, without the executor needing to be
data-driven. ``test_plan_matches_executed_phases`` pins the two together, so a
phase added to a matrix and missed in the table fails the tests.
"""
from __future__ import annotations

from dataclasses import dataclass

from .config import RunConfig
from .filesystems import BSFS_FILESYSTEMS
from .safety import cyclic_schedule


COMMON_FILESYSTEMS = (
    *BSFS_FILESYSTEMS, "old-bsfs", "ext4", "btrfs", "zfs", "zfs-4k"
)
KERNEL_FILESYSTEMS = (*BSFS_FILESYSTEMS, "ext4", "btrfs", "zfs", "zfs-4k")

COMMON_SCENARIO_PHASES: dict[str, tuple[str, ...]] = {
    "bulk": (
        "prepare",
        "cold-read",
        "warm-read",
        "random-read",
        "random-write",
        "verify",
    ),
    "metadata": (
        "create",
        "stat-cold",
        "stat-warm",
        "iterate",
        "rename",
        "unlink",
        "rmdir",
        "verify",
    ),
    "mixed": ("create", "read", "edit", "verify"),
}

# one tuple per fresh filesystem: old-bsfs retention and restore cannot share a
# device image, so they are two separate mkfs cycles
RECOVERY_PHASES: dict[str, tuple[tuple[str, tuple[str, ...]], ...]] = {
    **{
        filesystem: (("recovery-pressure", ("run", "verify")),)
        for filesystem in BSFS_FILESYSTEMS
    },
    "old-bsfs": (
        ("old-recovery", ("prepare", "retention")),
        ("old-recovery", ("prepare", "restore", "purge")),
    ),
}

RECOVERY_FILESYSTEMS = (*BSFS_FILESYSTEMS, "old-bsfs")

CONCURRENCY_SCENARIO_PHASES: dict[str, tuple[str, ...]] = {
    "concurrency-seq": ("write", "read"),
    "concurrency-mixed": ("create", "read", "edit", "verify"),
}

CONCURRENCY_WORKERS = (1, 4)


@dataclass(frozen=True)
class PlannedUnit:
    """one fresh_filesystem cycle: condition, mkfs, mount, phases, unmount."""

    matrix: str
    filesystem: str
    scenario: str
    repetition: int
    workers: int
    phases: tuple[str, ...]


@dataclass(frozen=True)
class RunPlan:
    units: tuple[PlannedUnit, ...]

    @property
    def total_units(self) -> int:
        return len(self.units)

    @property
    def total_phases(self) -> int:
        return sum(len(unit.phases) for unit in self.units)

    @property
    def filesystems(self) -> tuple[str, ...]:
        """in first-execution order, which is what the progress counter uses."""
        return tuple(dict.fromkeys(unit.filesystem for unit in self.units))

    def phases_for(self, filesystem: str) -> int:
        return sum(
            len(unit.phases)
            for unit in self.units
            if unit.filesystem == filesystem
        )

    def units_for(self, filesystem: str) -> int:
        return sum(1 for unit in self.units if unit.filesystem == filesystem)


def build_plan(config: RunConfig, filesystems: tuple[str, ...]) -> RunPlan:
    units: list[PlannedUnit] = []

    if config.publication:
        for filesystem in filesystems:
            units.append(
                PlannedUnit(
                    "validation",
                    filesystem,
                    "bulk",
                    0,
                    1,
                    COMMON_SCENARIO_PHASES["bulk"],
                )
            )

    for repetition in range(config.repetitions):
        for filesystem in cyclic_schedule(filesystems, repetition):
            for scenario in config.common_scenarios:
                units.append(
                    PlannedUnit(
                        "common",
                        filesystem,
                        scenario,
                        repetition,
                        1,
                        COMMON_SCENARIO_PHASES.get(scenario, ()),
                    )
                )

    for repetition in range(config.repetitions):
        for filesystem in RECOVERY_FILESYSTEMS:
            if filesystem not in filesystems:
                continue
            for scenario, phases in RECOVERY_PHASES[filesystem]:
                units.append(
                    PlannedUnit(
                        "recovery", filesystem, scenario, repetition, 1, phases
                    )
                )

    kernel = tuple(
        filesystem
        for filesystem in filesystems
        if filesystem in KERNEL_FILESYSTEMS
    )
    for repetition in range(config.repetitions):
        for filesystem in cyclic_schedule(kernel, repetition):
            for scenario in config.concurrency_scenarios:
                for workers in CONCURRENCY_WORKERS:
                    if len(config.concurrency_cpus) < workers:
                        continue
                    units.append(
                        PlannedUnit(
                            "concurrency",
                            filesystem,
                            scenario,
                            repetition,
                            workers,
                            CONCURRENCY_SCENARIO_PHASES.get(scenario, ()),
                        )
                    )

    return RunPlan(tuple(units))
