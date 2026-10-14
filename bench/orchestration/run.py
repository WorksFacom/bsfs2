#!/usr/bin/env python3
from __future__ import annotations

import argparse
import dataclasses
from contextlib import contextmanager
from datetime import datetime, timezone
import os
from pathlib import Path
import signal
import sys
from typing import Iterator, Mapping, Sequence

if __package__ in {None, ""}:
    # run as a script: make the relative imports below resolve
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    import bench.orchestration  # noqa: F401

    __package__ = "bench.orchestration"

from .analyze import analyze_run
from .cache import cold_reset, reopen
from .command import CommandError, CommandRunner
from .conditioning import DeviceConditioner
from .config import RunConfig, load_config
from .filesystems import (
    FilesystemAdapter,
    FilesystemContext,
    FilesystemUnavailable,
    bsfs_targets_by_strategy,
    create_adapter,
)
from .governor import PerformanceGovernor
from .manifest import collect_manifest, write_manifest
from .modules import (
    BsfsModuleError,
    CustomBsfsArtifacts,
    build_module_argv,
    build_modules,
    define_custom_artifacts,
    stage_custom_artifacts,
)
from .plan import (
    COLD,
    CONCURRENCY_WORKERS,
    FILESYSTEMS,
    REOPEN,
    PlannedUnit,
    RunPlan,
    build_plan,
)
from .preflight import BenchmarkBinaryError, validate_benchmark_binaries
from .progress import Level, RunReporter
from .safety import (
    DeviceBusy,
    DeviceInfo,
    SafetyError,
    cyclic_schedule,
    device_mountpoints,
    inspect_physical_device,
    validate_loop_image,
)
from .udev import UdisksIgnore, settle

# mkfs, mount, umount and friends. workload phases use the configured phase timeout instead
HELPER_TIMEOUT_S = 300
# recovery rewrites the whole device several times
RECOVERY_TIMEOUT_FACTOR = 3


def sudo_invoker_owner(
    environ: Mapping[str, str] | None = None,
) -> tuple[int, int] | None:
    """uid/gid of the user who ran sudo, or None."""
    environment = os.environ if environ is None else environ
    uid_text = environment.get("SUDO_UID")
    gid_text = environment.get("SUDO_GID")
    if uid_text is None or gid_text is None:
        return None
    try:
        uid = int(uid_text)
        gid = int(gid_text)
    except ValueError:
        return None
    if uid < 0 or gid < 0:
        return None
    return uid, gid


def transfer_result_ownership(
    run_dir: Path, owner: tuple[int, int]
) -> list[str]:
    """Chowns a finished result tree back to the user who ran sudo.

    returns the paths it couldn't chown, so one bad entry doesn't strand the rest under root. symlinks
    aren't followed and mounted dirs are skipped, since the mountpoint lives under run_dir and a failed
    umount would otherwise send this chown across the device under test.
    """
    if not run_dir.exists():
        return []
    uid, gid = owner
    failed: list[str] = []

    def transfer(path: Path) -> None:
        try:
            os.chown(path, uid, gid, follow_symlinks=False)
        except OSError as error:
            failed.append(f"{path}: {error}")

    transfer(run_dir)
    for parent, directories, files in os.walk(run_dir):
        base = Path(parent)
        directories[:] = [
            name for name in directories if not os.path.ismount(base / name)
        ]
        for name in (*directories, *files):
            transfer(base / name)
    return failed


def custom_bsfs_definitions(
    repository: Path,
    values: Sequence[Sequence[str]] | None,
) -> dict[str, CustomBsfsArtifacts]:
    """Parses and validates the repeatable ``--custom-bsfs`` triples."""
    definitions: dict[str, CustomBsfsArtifacts] = {}
    for name, module, formatter in values or ():
        if name in FILESYSTEMS:
            raise BsfsModuleError(
                f"custom BSFS name {name!r} collides with a built-in target"
            )
        if name in definitions:
            raise BsfsModuleError(
                f"custom BSFS name {name!r} was supplied more than once"
            )
        definitions[name] = define_custom_artifacts(
            repository,
            name,
            Path(module),
            Path(formatter),
        )
    return definitions


class BenchmarkRun:
    def __init__(
        self,
        *,
        repository: Path,
        run_dir: Path,
        config: RunConfig,
        runner: CommandRunner,
        device: str,
        stable_device: str,
        device_info: DeviceInfo | None,
        run_id: str,
        reporter: RunReporter | None = None,
        custom_bsfs: Mapping[str, CustomBsfsArtifacts] | None = None,
    ):
        self.reporter = reporter if reporter is not None else RunReporter(
            level=Level.QUIET
        )
        self.repository = repository
        self.run_dir = run_dir
        self.config = config
        self.runner = runner
        self.device = device
        self.stable_device = stable_device
        self.device_info = device_info
        self.run_id = run_id
        self.custom_bsfs = dict(custom_bsfs or {})
        self.mountpoint = run_dir / "mnt"
        self.results = run_dir / "raw" / "results.jsonl"
        self.artifacts = run_dir / "raw" / "artifacts"
        self.current_adapter: FilesystemAdapter | None = None
        self.failures: list[dict[str, object]] = []
        self.versions: dict[str, str] = {}
        if not runner.dry_run:
            self.results.parent.mkdir(parents=True, exist_ok=True)
            self.artifacts.mkdir(parents=True, exist_ok=True)

    def context_for(self, filesystem: str) -> FilesystemContext:
        return FilesystemContext(
            runner=self.runner,
            repository=self.repository,
            device=self.device,
            stable_device=self.stable_device,
            mountpoint=self.mountpoint,
            run_id=f"{self.run_id}_{filesystem}",
            reporter=self.reporter,
        )

    def filesystem_version(self, adapter: FilesystemAdapter) -> str:
        """Cached per filesystem, the tools don't change mid-run."""
        if adapter.name not in self.versions:
            self.versions[adapter.name] = adapter.version()
        return self.versions[adapter.name]

    def cleanup_current(self) -> None:
        if self.current_adapter is None:
            return
        try:
            with self.reporter.timed("unmount"):
                self.current_adapter.unmount()
        finally:
            self.current_adapter = None

    def assert_device_free(self) -> None:
        if self.runner.dry_run:
            return
        foreign = [
            point
            for point in device_mountpoints(self.device)
            if point != str(self.mountpoint)
        ]
        if foreign:
            raise DeviceBusy(
                f"{self.device} is mounted at {', '.join(foreign)}; "
                "something outside the bench grabbed it"
            )

    @contextmanager
    def fresh_filesystem(self, filesystem: str) -> Iterator[FilesystemAdapter]:
        self.cleanup_current()
        self.assert_device_free()
        adapter = create_adapter(
            filesystem, self.context_for(filesystem), self.custom_bsfs
        )
        self.current_adapter = adapter
        conditioner = DeviceConditioner(
            self.runner,
            self.device,
            self.device_info,
            self.config.discard,
        )
        try:
            with self.reporter.timed(
                "condition device", conditioner.discard_mode
            ):
                conditioner.condition()
            with self.reporter.timed(f"mkfs {adapter.name}"):
                adapter.format()
                settle(self.runner, f"{adapter.name}-udev-settle")
            with self.reporter.timed(
                "mount", adapter.mount_options or None
            ):
                adapter.mount()
            yield adapter
            adapter.validate_health()
        finally:
            self.cleanup_current()

    def _runner_argv(
        self,
        adapter: FilesystemAdapter,
        scenario: str,
        phase: str,
        repetition: int,
        seed: int,
        workers: int,
    ) -> list[str]:
        argv = [
            str(self.repository / "bench/bin/bench-posix"),
            "--root",
            str(self.mountpoint),
            "--scenario",
            scenario,
            "--phase",
            phase,
            "--seed",
            str(seed),
            "--repetition",
            str(repetition),
            "--workers",
            str(workers),
            "--results",
            str(self.results),
            "--artifact-dir",
            str(self.artifacts),
            "--block-device",
            self.device,
            "--filesystem",
            adapter.name,
            "--filesystem-version",
            self.filesystem_version(adapter),
            "--filesystem-options",
            adapter.mount_options,
            "--run-id",
            self.run_id,
        ]
        if scenario == "recovery-pressure":
            argv.extend(
                [
                    "--reclaim-stats",
                    f"/sys/kernel/debug/bsfs/{Path(self.device).name}/"
                    "reclaim_stats",
                ]
            )
        if scenario.startswith("concurrency-"):
            cpus = self.config.concurrency_cpus[:workers]
            argv.extend(
                ["--cpu-list", ",".join(str(cpu) for cpu in cpus)]
            )
        else:
            argv.extend(["--cpu", str(self.config.common_cpu)])
        return argv

    def run_phase(
        self,
        adapter: FilesystemAdapter,
        scenario: str,
        phase: str,
        repetition: int,
        seed: int,
        workers: int = 1,
    ) -> None:
        timeout = self.config.phase_timeout_s
        if scenario == "recovery-pressure":
            timeout *= RECOVERY_TIMEOUT_FACTOR
        with self.reporter.phase(
            filesystem=adapter.name,
            scenario=scenario,
            phase=phase,
            workers=workers,
            results=None if self.runner.dry_run else self.results,
        ):
            self.runner.run(
                self._runner_argv(
                    adapter, scenario, phase, repetition, seed, workers
                ),
                cwd=self.repository,
                label=(
                    f"{adapter.name}-{scenario}-{phase}-r{repetition}-w{workers}"
                ),
                timeout=timeout,
            )

    def record_failure(
        self,
        filesystem: str,
        scenario: str,
        repetition: int,
        error: Exception,
    ) -> None:
        self.failures.append(
            {
                "filesystem": filesystem,
                "scenario": scenario,
                "repetition": repetition,
                "error": str(error),
            }
        )
        log_hint = (
            str(self.runner.last_error_log)
            if isinstance(error, CommandError)
            and self.runner.last_error_log is not None
            else None
        )
        self.reporter.failure(filesystem, scenario, repetition, error, log_hint)

    def run_unit(self, unit: PlannedUnit) -> None:
        """One mkfs cycle. a failure is recorded and the run moves on to the next unit."""
        seed = self.config.seeds[unit.repetition]
        self.reporter.unit_start(
            matrix=unit.matrix,
            filesystem=unit.filesystem,
            scenario=unit.scenario,
            repetition=unit.repetition,
            workers=unit.workers,
        )
        try:
            with self.fresh_filesystem(unit.filesystem) as adapter:
                for step in unit.steps:
                    if step == COLD:
                        cold_reset(adapter)
                    elif step == REOPEN:
                        reopen(adapter)
                    else:
                        self.run_phase(
                            adapter,
                            unit.scenario,
                            step,
                            unit.repetition,
                            seed,
                            unit.workers,
                        )
        except DeviceBusy:
            raise
        except (CommandError, FilesystemUnavailable, RuntimeError) as error:
            self.record_failure(
                unit.filesystem, unit.scenario, unit.repetition, error
            )

    def run_plan(self, plan: RunPlan) -> None:
        for unit in plan.units:
            try:
                self.run_unit(unit)
            except DeviceBusy as error:
                self.record_failure(
                    unit.filesystem, unit.scenario, unit.repetition, error
                )
                self.reporter.warning(f"aborting run: {error}")
                return


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--device")
    target.add_argument("--loop-image", type=Path)
    parser.add_argument("--confirm")
    parser.add_argument("--allow-size-outside-range", action="store_true")
    parser.add_argument("--allow-shared-disk", action="store_true")
    parser.add_argument("--run-dir", type=Path)
    parser.add_argument("--filesystems", nargs="+")
    parser.add_argument(
        "--custom-bsfs",
        action="append",
        nargs=3,
        metavar=("NAME", "MODULE", "MKFS"),
        help=(
            "register a named BSFS target backed by a matching prebuilt "
            "bsfs2.ko and mkfs.bsfs pair; repeat for multiple targets"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--recovery",
        action=argparse.BooleanOptionalAction,
        default=None,
        help=(
            "also run the bsfs-only recovery-pressure scenario, overriding "
            "[run] recovery in the config"
        ),
    )
    verbosity = parser.add_mutually_exclusive_group()
    verbosity.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="also report every subprocess with its duration",
    )
    verbosity.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="only report failures and the closing summary",
    )
    parser.add_argument(
        "--no-color", action="store_true", help="never colourise progress output"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repository = Path(__file__).resolve().parents[2]
    config = load_config(args.config.resolve())
    if getattr(args, "recovery", None) is not None:
        config = dataclasses.replace(config, recovery=args.recovery)
    try:
        custom_bsfs = custom_bsfs_definitions(
            repository, getattr(args, "custom_bsfs", None)
        )
    except BsfsModuleError as error:
        print(f"benchmark setup failed: {error}", file=sys.stderr)
        return 2
    selected = tuple(args.filesystems or config.filesystems)
    unknown = set(selected) - {*FILESYSTEMS, *custom_bsfs}
    if unknown:
        message = f"unknown filesystems: {', '.join(sorted(unknown))}"
        if any(name.startswith("bsfs-") for name in unknown):
            message += (
                "; custom BSFS targets must also be defined with "
                "--custom-bsfs NAME MODULE MKFS"
            )
        raise SystemExit(message)
    selected_custom_bsfs = {
        name: custom_bsfs[name] for name in selected if name in custom_bsfs
    }
    bsfs_targets = bsfs_targets_by_strategy(selected)
    bsfs_strategies = tuple(bsfs_targets)
    if args.quiet:
        level = Level.QUIET
    elif args.verbose:
        level = Level.VERBOSE
    else:
        level = Level.NORMAL
    reporter = RunReporter(
        level=level, color=False if args.no_color else None
    )
    if not args.dry_run:
        try:
            validate_benchmark_binaries(repository)
        except BenchmarkBinaryError as error:
            print(f"benchmark setup failed: {error}", file=sys.stderr)
            return 2
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = (
        args.run_dir.resolve()
        if args.run_dir
        else repository / "bench/results" / run_id
    )
    if run_dir.exists() and any(run_dir.iterdir()):
        raise SystemExit(f"run directory is not empty: {run_dir}")
    if not args.dry_run and os.geteuid() != 0:
        raise SystemExit("actual loop or physical runs require root")
    reporter.header(
        run_id=run_id,
        config_path=args.config.resolve(),
        repetitions=config.repetitions,
        seeds=config.seeds,
        common_cpu=config.common_cpu,
        concurrency_cpus=config.concurrency_cpus,
        run_dir=run_dir,
        dry_run=args.dry_run,
    )
    plan = build_plan(config, selected, custom_bsfs=tuple(selected_custom_bsfs))
    reporter.announce_plan(plan)
    if len(config.concurrency_cpus) < max(CONCURRENCY_WORKERS):
        reporter.warning(
            f"only {len(config.concurrency_cpus)} concurrency CPUs configured; "
            "larger worker counts are skipped"
        )
    reporter.section("preflight")
    if not args.dry_run:
        reporter.step("benchmark binaries ok")
    for strategy, targets in bsfs_targets.items():
        if len(targets) > 1:
            reporter.warning(
                f"{' and '.join(targets)} use the same {strategy} build; the "
                "runner measures it under each label"
            )
    stashed_bsfs_modules: tuple[Path, ...] = ()
    if not args.dry_run and bsfs_strategies:
        try:
            with reporter.timed(
                "build and probe-load the bsfs modules",
                ", ".join(bsfs_strategies),
            ):
                stashed_bsfs_modules = build_modules(repository, bsfs_strategies)
        except BsfsModuleError as error:
            print(f"benchmark setup failed: {error}", file=sys.stderr)
            return 2
    if not args.dry_run and selected_custom_bsfs:
        try:
            staged: dict[str, CustomBsfsArtifacts] = {}
            with reporter.timed(
                "stage and probe-load custom bsfs artifacts",
                ", ".join(selected_custom_bsfs),
            ):
                for name, artifacts in selected_custom_bsfs.items():
                    staged[name] = stage_custom_artifacts(artifacts)
            selected_custom_bsfs = staged
        except BsfsModuleError as error:
            print(f"benchmark setup failed: {error}", file=sys.stderr)
            return 2
    runner = CommandRunner(
        run_dir,
        dry_run=args.dry_run,
        reporter=reporter,
        default_timeout=HELPER_TIMEOUT_S,
    )
    result_owner = sudo_invoker_owner()
    device_info: DeviceInfo | None = None
    loop_device: str | None = None
    udisks_ignore: UdisksIgnore | None = None
    benchmark: BenchmarkRun | None = None

    try:
        if args.device:
            device_info = inspect_physical_device(
                args.device,
                args.confirm,
                repository=repository,
                min_size=config.min_device_gib * 1024**3,
                max_size=config.max_device_gib * 1024**3,
                allow_size_override=args.allow_size_outside_range,
                allow_shared_disk=args.allow_shared_disk,
            )
            device = device_info.resolved
            stable_device = device_info.requested
            reporter.target(
                f"device {device_info.requested} -> {device_info.resolved} "
                f"(parent={device_info.parent}, model={device_info.model!r}, "
                f"serial={device_info.serial!r}, "
                f"{device_info.size_bytes / 1024**3:.1f} GiB)"
            )
        else:
            assert args.loop_image is not None
            validate_loop_image(args.loop_image, args.confirm)
            if args.dry_run:
                device = "/dev/loop-dry-run"
            else:
                args.loop_image.parent.mkdir(parents=True, exist_ok=True)
                with args.loop_image.open("wb") as image:
                    image.truncate(config.loop_size_mib * 1024**2)
                result = runner.run(
                    ["losetup", "--find", "--show", str(args.loop_image)],
                    label="attach-loop-device",
                )
                device = result.stdout.strip()
                if not device.startswith("/dev/loop"):
                    raise RuntimeError("losetup did not return a loop device")
                loop_device = device
            stable_device = device
            reporter.target(
                f"loop {args.loop_image} -> {device} "
                f"({config.loop_size_mib} MiB)"
            )

        udisks_ignore = UdisksIgnore(runner, device, run_id)
        udisks_ignore.install()

        # dry runs skip preflight, so show the builds it would do
        if args.dry_run:
            for strategy in bsfs_strategies:
                runner.run(
                    build_module_argv(repository, strategy),
                    label=f"build-current-bsfs-{strategy}",
                )
        debugfs = runner.run(
            ["mountpoint", "--quiet", "/sys/kernel/debug"],
            check=False,
            label="check-debugfs",
        )
        if debugfs.returncode != 0:
            with reporter.timed("mount debugfs"):
                runner.run(
                    ["mount", "-t", "debugfs", "debugfs", "/sys/kernel/debug"],
                    label="mount-debugfs",
                )
        else:
            reporter.step("debugfs already mounted")
        kernel_before = runner.run(
            ["dmesg", "--color=never"],
            check=False,
            label="kernel-log-before",
        )
        reporter.step("kernel log captured (before)")

        manifest_device: DeviceInfo | dict[str, object]
        if device_info:
            manifest_device = device_info
        else:
            manifest_device = {
                "mode": "loop",
                "path": str(args.loop_image),
                "resolved_device": device,
                "size_bytes": config.loop_size_mib * 1024**2,
            }
        benchmark = BenchmarkRun(
            repository=repository,
            run_dir=run_dir,
            config=config,
            runner=runner,
            device=device,
            stable_device=stable_device,
            device_info=device_info,
            run_id=run_id,
            reporter=reporter,
            custom_bsfs=selected_custom_bsfs,
        )
        custom_staged_paths = tuple(
            path
            for artifacts in selected_custom_bsfs.values()
            for path in (artifacts.module, artifacts.formatter)
        )
        manifest = collect_manifest(
            repository=repository,
            config_path=args.config.resolve(),
            config=config,
            run_id=run_id,
            device=manifest_device,
            affinity=(config.common_cpu, *config.concurrency_cpus),
            filesystems=selected,
            extra_artifacts=(*stashed_bsfs_modules, *custom_staged_paths),
            custom_bsfs=selected_custom_bsfs,
        )
        manifest["size_range_override_used"] = bool(
            args.device and args.allow_size_outside_range
        )
        manifest["shared_disk_override_used"] = bool(
            args.device and args.allow_shared_disk
        )
        manifest["filesystem_schedule"] = [
            {
                "repetition": repetition,
                "order": list(cyclic_schedule(selected, repetition)),
            }
            for repetition in range(config.repetitions)
        ]
        # built from the adapters so it can't drift from what actually runs
        filesystem_configuration: dict[str, object] = {}
        for filesystem in selected:
            try:
                adapter = create_adapter(
                    filesystem,
                    benchmark.context_for(filesystem),
                    selected_custom_bsfs,
                )
            except (FilesystemUnavailable, ValueError) as error:
                filesystem_configuration[filesystem] = {"unavailable": str(error)}
                continue
            filesystem_configuration[filesystem] = {
                **adapter.configuration(),
                "version": benchmark.filesystem_version(adapter),
            }
        manifest["filesystem_configuration"] = filesystem_configuration
        for filesystem, entry in filesystem_configuration.items():
            if isinstance(entry, dict) and "unavailable" in entry:
                reporter.warning(
                    f"{filesystem} is unavailable: {entry['unavailable']}"
                )
        manifest["kernel_log"] = {
            "path": "raw/kernel.log",
            "capture_before_returncode": kernel_before.returncode,
        }
        if not args.dry_run:
            write_manifest(run_dir, manifest)

        def interrupt(signum: int, frame: object) -> None:
            del signum, frame
            raise KeyboardInterrupt

        previous_handlers = {
            sig: signal.signal(sig, interrupt)
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
        }
        try:
            cpus = tuple(
                dict.fromkeys(
                    (config.common_cpu, *config.concurrency_cpus)
                )
            )
            reporter.section("matrix")
            with PerformanceGovernor(runner, cpus):
                benchmark.run_plan(plan)
        finally:
            benchmark.cleanup_current()
            for sig, previous in previous_handlers.items():
                signal.signal(sig, previous)
        reporter.end_section()

        manifest["failures"] = benchmark.failures
        manifest["completed_at"] = datetime.now(timezone.utc).isoformat()
        kernel_after = runner.run(
            ["dmesg", "--color=never"],
            check=False,
            label="kernel-log-after",
        )
        before = kernel_before.stdout
        after = kernel_after.stdout
        excerpt = after[len(before) :] if after.startswith(before) else after
        manifest["kernel_log"]["capture_after_returncode"] = (
            kernel_after.returncode
        )
        manifest["kernel_log"]["initial_log_rotated"] = not after.startswith(
            before
        )
        severe = (
            "BUG:",
            "Oops:",
            "kernel BUG at",
            "I/O error",
            "Buffer I/O error",
            "blk_update_request",
        )
        bsfs_problem = ("failed", "leak", "corrupt", "invalid", "unable")
        findings = [
            line
            for line in excerpt.splitlines()
            if any(token in line for token in severe)
            or (
                ("bsfs2:" in line or "BSFS:" in line)
                and any(token in line.lower() for token in bsfs_problem)
            )
        ]
        manifest["kernel_log"]["findings"] = findings
        if findings:
            benchmark.record_failure(
                "host",
                "kernel-log-validation",
                config.repetitions - 1,
                RuntimeError(
                    f"{len(findings)} relevant kernel warning/error lines"
                ),
            )
        if kernel_after.returncode != 0:
            benchmark.record_failure(
                "host",
                "kernel-log-capture",
                config.repetitions - 1,
                RuntimeError("could not capture the post-run kernel log"),
            )
        if not args.dry_run:
            kernel_log = run_dir / "raw" / "kernel.log"
            kernel_log.parent.mkdir(parents=True, exist_ok=True)
            kernel_log.write_text(excerpt, encoding="utf-8")
            write_manifest(run_dir, manifest)
        report: Path | None = None
        if not args.dry_run and benchmark.results.exists():
            report = analyze_run(run_dir)
        reporter.summary(
            run_id=run_id,
            failures=benchmark.failures,
            kernel_findings=len(findings),
            run_dir=run_dir,
            report=report,
        )
        return 1 if benchmark.failures else 0
    except (
        SafetyError,
        CommandError,
        FilesystemUnavailable,
        RuntimeError,
        OSError,
    ) as error:
        print(f"benchmark setup failed: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("benchmark interrupted; cleanup completed", file=sys.stderr)
        reporter.summary(
            run_id=run_id,
            failures=benchmark.failures if benchmark else [],
            kernel_findings=0,
            run_dir=run_dir,
            interrupted=True,
        )
        return 130
    finally:
        if loop_device:
            runner.run(
                ["losetup", "--detach", loop_device],
                check=False,
                label="detach-loop-device",
            )
        if udisks_ignore:
            udisks_ignore.remove()
        if result_owner:
            unowned = transfer_result_ownership(run_dir, result_owner)
            if unowned:
                print(
                    f"benchmark result ownership transfer failed for "
                    f"{len(unowned)} paths, first: {unowned[0]}",
                    file=sys.stderr,
                )


if __name__ == "__main__":
    raise SystemExit(main())
