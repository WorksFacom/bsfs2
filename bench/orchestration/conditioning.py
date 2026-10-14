from __future__ import annotations

from .command import CommandRunner
from .safety import DeviceInfo
from .udev import settle


class DeviceConditioner:
    def __init__(
        self,
        runner: CommandRunner,
        device: str,
        info: DeviceInfo | None,
        discard_requested: bool,
    ):
        self.runner = runner
        self.device = device
        self.info = info
        self.discard_enabled = bool(
            discard_requested and info and info.discard_max_bytes > 0
        )
        self.discard_mode = "discard" if self.discard_enabled else "no-discard"

    def condition(self) -> None:
        self.runner.run(
            ["blockdev", "--flushbufs", self.device],
            label="condition-flush-device",
        )
        self.runner.run(
            ["wipefs", "--all", self.device], label="condition-wipe-signatures"
        )
        if self.discard_enabled:
            # a failed discard aborts the run rather than mixing conditions
            self.runner.run(
                ["blkdiscard", self.device], label="condition-discard-device"
            )
        settle(self.runner, "condition-udev-settle")

