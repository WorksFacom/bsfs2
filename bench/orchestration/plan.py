"""what a run executes: one step table per scenario, expanded into the ordered list of units.

``run.py`` walks the same units, so the totals shown before the first mkfs can't drift from what runs.
"""
from __future__ import annotations

from dataclasses import dataclass

from .config import RunConfig
from .filesystems import BSFS_FILESYSTEMS
from .safety import cyclic_schedule


FILESYSTEMS = (*BSFS_FILESYSTEMS, "ext4", "btrfs", "zfs", "zfs-4k")

# cache transitions between phases, see cache.py
COLD = "@cold-reset"
REOPEN = "@reopen"

# without a drop before the cold phases, reads and lookups hit the page and dentry caches
# and every filesystem looks the same
SCENARIOS: dict[str, tuple[str, ...]] = {
    "bulk": (
        "prepare",
        COLD,
        "cold-read",
        "warm-read",
        COLD,
        "random-read",
        "random-write",
        REOPEN,
        "verify",
    ),
    "metadata": (
        "create",
        COLD,
        "stat-cold",
        "stat-warm",
        COLD,
        "iterate",
        "rename",
        "unlink",
        "rmdir",
        REOPEN,
        "verify",
    ),
    "mixed": ("create", COLD, "read", "edit", REOPEN, "verify"),
    "concurrency-seq": ("write", COLD, "read"),
    "concurrency-mixed": ("create", COLD, "read", "edit", REOPEN, "verify"),
    "recovery-pressure": ("run", REOPEN, "verify"),
}

# (workers, work_scale). 1x4 is the total work of 4x1 on one worker, so 1x1 to 1x4 is the
# dataset-size effect and 1x4 to 4x1 is the worker-count effect at a fixed total
CONCURRENCY_SHAPES = ((1, 1), (1, 4), (4, 1))
CONCURRENCY_WORKERS = tuple(sorted({workers for workers, _ in CONCURRENCY_SHAPES}))


@dataclass(frozen=True)
class PlannedUnit:
    """one fresh_filesystem cycle: condition, mkfs, mount, steps, unmount."""

    matrix: str
    filesystem: str
    scenario: str
    repetition: int
    workers: int
    steps: tuple[str, ...]
    work_scale: int = 1

    @property
    def phases(self) -> tuple[str, ...]:
        return tuple(step for step in self.steps if not step.startswith("@"))


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
        """in first-execution order, which the progress counter relies on."""
        return tuple(dict.fromkeys(unit.filesystem for unit in self.units))

    def phases_for(self, filesystem: str) -> int:
        return sum(
            len(unit.phases)
            for unit in self.units
            if unit.filesystem == filesystem
        )

    def units_for(self, filesystem: str) -> int:
        return sum(1 for unit in self.units if unit.filesystem == filesystem)


def _steps(scenario: str) -> tuple[str, ...]:
    try:
        return SCENARIOS[scenario]
    except KeyError as error:
        raise ValueError(f"unknown scenario: {scenario}") from error


def build_plan(
    config: RunConfig,
    filesystems: tuple[str, ...],
    *,
    custom_bsfs: tuple[str, ...] = (),
) -> RunPlan:
    units: list[PlannedUnit] = []

    for repetition in range(config.repetitions):
        for filesystem in cyclic_schedule(filesystems, repetition):
            for scenario in config.common_scenarios:
                units.append(
                    PlannedUnit(
                        "common", filesystem, scenario, repetition, 1, _steps(scenario)
                    )
                )

    for repetition in range(config.repetitions):
        for filesystem in cyclic_schedule(filesystems, repetition):
            for scenario in config.concurrency_scenarios:
                for workers, work_scale in CONCURRENCY_SHAPES:
                    if len(config.concurrency_cpus) < workers:
                        continue
                    units.append(
                        PlannedUnit(
                            "concurrency",
                            filesystem,
                            scenario,
                            repetition,
                            workers,
                            _steps(scenario),
                            work_scale,
                        )
                    )

    # last and once per target: it rewrites the whole device several times and has nothing to compare against
    if config.recovery:
        for filesystem in (*BSFS_FILESYSTEMS, *custom_bsfs):
            if filesystem not in filesystems:
                continue
            units.append(
                PlannedUnit(
                    "recovery",
                    filesystem,
                    "recovery-pressure",
                    0,
                    1,
                    _steps("recovery-pressure"),
                )
            )

    return RunPlan(tuple(units))
