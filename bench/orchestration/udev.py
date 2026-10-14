from __future__ import annotations

from pathlib import Path

from .command import CommandRunner

RULES_DIR = Path("/run/udev/rules.d")


def settle(runner: CommandRunner, label: str) -> None:
    """udevd holds LOCK_SH while probing a freshly written device, which a locking mkfs loses to."""
    runner.run(["udevadm", "settle", "--timeout=30"], label=label)


class UdisksIgnore:
    """Transient rule hiding the target from udisks, so desktop automounters leave it alone."""

    def __init__(self, runner: CommandRunner, device: str, run_id: str):
        self.runner = runner
        self.device = device
        safe = "".join(char if char.isalnum() or char in "-_" else "_" for char in run_id)
        self.rule = RULES_DIR / f"99-bsfs-bench-{safe}.rules"

    def install(self) -> None:
        if not self.runner.dry_run:
            self.rule.parent.mkdir(parents=True, exist_ok=True)
        self.runner.write_control(
            self.rule,
            f'KERNEL=="{Path(self.device).resolve().name}", ENV{{UDISKS_IGNORE}}="1"\n',
            label="udisks-ignore-rule",
        )
        self.runner.run(["udevadm", "control", "--reload"], label="udev-reload-rules")
        # the rule only lands on the next event, and udisks must see HintIgnore before the first mkfs
        self.runner.run(
            ["udevadm", "trigger", "--action=change", "--settle", self.device],
            label="udev-apply-udisks-ignore",
        )

    def remove(self) -> None:
        if not self.runner.dry_run:
            self.rule.unlink(missing_ok=True)
        # no re-trigger, that would invite an automount of whatever fs was left on the device
        self.runner.run(
            ["udevadm", "control", "--reload"], check=False, label="udev-reload-rules"
        )
