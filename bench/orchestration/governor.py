from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .command import CommandRunner


@dataclass
class GovernorState:
    path: Path
    value: str


class PerformanceGovernor:
    def __init__(self, runner: CommandRunner, cpus: tuple[int, ...]):
        self.runner = runner
        self.cpus = cpus
        self.states: list[GovernorState] = []

    def _restore(self) -> None:
        first_error: Exception | None = None
        for state in reversed(self.states):
            try:
                self.runner.write_control(
                    state.path, state.value + "\n", "restore-governor"
                )
            except OSError as error:
                if first_error is None:
                    first_error = error
        self.states.clear()
        if first_error is not None:
            raise first_error

    def __enter__(self) -> "PerformanceGovernor":
        policies: set[Path] = set()
        for cpu in self.cpus:
            policy_link = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
            if policy_link.exists():
                policies.add(policy_link.resolve())
        for policy in sorted(policies):
            governor = policy / "scaling_governor"
            value = governor.read_text(encoding="ascii").strip()
            self.states.append(GovernorState(governor, value))
            try:
                self.runner.write_control(
                    governor, "performance\n", "set-governor"
                )
            except OSError:
                self._restore()
                raise
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self._restore()
