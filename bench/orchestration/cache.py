from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import time
from typing import Iterator

from .command import CommandRunner
from .filesystems import FilesystemAdapter, OldBsfsAdapter, ZfsAdapter


@contextmanager
def _reported(adapter: FilesystemAdapter, description: str) -> Iterator[None]:
    """Times the transition and reports it once the device is quiet again."""
    started = time.monotonic()
    try:
        yield
    finally:
        reporter = getattr(adapter.context, "reporter", None)
        if reporter is not None:
            reporter.cache_event(description, time.monotonic() - started)


def drop_linux_caches(runner: CommandRunner, device: str) -> None:
    runner.run(["blockdev", "--flushbufs", device], label="flush-block-device")
    runner.write_control(
        Path("/proc/sys/vm/drop_caches"), "3\n", "drop-linux-page-caches"
    )


def cold_reset(adapter: FilesystemAdapter) -> None:
    context = adapter.context
    if isinstance(adapter, OldBsfsAdapter):
        with _reported(adapter, "flush device"):
            drop_linux_caches(context.runner, context.device)
        return
    if isinstance(adapter, ZfsAdapter):
        with _reported(adapter, "export + drop_caches + import"):
            adapter.unmount()
            drop_linux_caches(context.runner, context.device)
            adapter.import_pool()
        return
    with _reported(adapter, "unmount + drop_caches + mount"):
        adapter.unmount()
        drop_linux_caches(context.runner, context.device)
        adapter.mount()


def reopen(adapter: FilesystemAdapter) -> None:
    context = adapter.context
    if isinstance(adapter, OldBsfsAdapter):
        with _reported(adapter, "reopen (flush)"):
            context.runner.run(
                ["blockdev", "--flushbufs", context.device],
                label="old-bsfs-reopen-flush",
            )
        return
    if isinstance(adapter, ZfsAdapter):
        with _reported(adapter, "reopen (export + import)"):
            adapter.unmount()
            adapter.import_pool()
        return
    with _reported(adapter, "reopen (unmount + mount)"):
        adapter.unmount()
        adapter.mount()
