from __future__ import annotations

from pathlib import Path

from .command import CommandRunner
from .filesystems import FilesystemAdapter, OldBsfsAdapter, ZfsAdapter


def drop_linux_caches(runner: CommandRunner, device: str) -> None:
    runner.run(["blockdev", "--flushbufs", device], label="flush-block-device")
    runner.write_control(
        Path("/proc/sys/vm/drop_caches"), "3\n", "drop-linux-page-caches"
    )


def cold_reset(adapter: FilesystemAdapter) -> None:
    context = adapter.context
    if isinstance(adapter, OldBsfsAdapter):
        drop_linux_caches(context.runner, context.device)
        return
    if isinstance(adapter, ZfsAdapter):
        adapter.unmount()
        drop_linux_caches(context.runner, context.device)
        adapter.import_pool()
        return
    adapter.unmount()
    drop_linux_caches(context.runner, context.device)
    adapter.mount()


def reopen(adapter: FilesystemAdapter) -> None:
    context = adapter.context
    if isinstance(adapter, OldBsfsAdapter):
        context.runner.run(
            ["blockdev", "--flushbufs", context.device],
            label="old-bsfs-reopen-flush",
        )
        return
    if isinstance(adapter, ZfsAdapter):
        adapter.unmount()
        adapter.import_pool()
        return
    adapter.unmount()
    adapter.mount()

