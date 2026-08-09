#!/usr/bin/env python3
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import sys
from typing import Iterator, Mapping

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from bench.orchestration.cache import cold_reset, reopen
    from bench.orchestration.command import CommandError, CommandRunner
    from bench.orchestration.conditioning import DeviceConditioner
    from bench.orchestration.config import RunConfig, load_config
    from bench.orchestration.filesystems import (
        FilesystemAdapter,
        FilesystemContext,
        FilesystemUnavailable,
        create_adapter,
    )
    from bench.orchestration.governor import PerformanceGovernor
    from bench.orchestration.manifest import collect_manifest, write_manifest
    from bench.orchestration.preflight import (
        BenchmarkBinaryError,
        CorrectnessGateError,
        run_correctness_gates,
        validate_benchmark_binaries,
    )
    from bench.orchestration.safety import (
        DeviceInfo,
        SafetyError,
        cyclic_schedule,
        inspect_physical_device,
        validate_loop_image,
    )
else:
    from .cache import cold_reset, reopen
    from .command import CommandError, CommandRunner
    from .conditioning import DeviceConditioner
    from .config import RunConfig, load_config
    from .filesystems import (
        FilesystemAdapter,
        FilesystemContext,
        FilesystemUnavailable,
        create_adapter,
    )
    from .governor import PerformanceGovernor
    from .manifest import collect_manifest, write_manifest
    from .preflight import (
        BenchmarkBinaryError,
        CorrectnessGateError,
        run_correctness_gates,
        validate_benchmark_binaries,
    )
    from .safety import (
        DeviceInfo,
        SafetyError,
        cyclic_schedule,
        inspect_physical_device,
        validate_loop_image,
    )


COMMON_FILESYSTEMS = ("bsfs", "old-bsfs", "ext4", "btrfs", "zfs", "zfs-4k")
KERNEL_FILESYSTEMS = ("bsfs", "ext4", "btrfs", "zfs", "zfs-4k")


def sudo_invoker_owner(
    environ: Mapping[str, str] | None = None,
) -> tuple[int, int] | None:
    """Return the uid/gid of the user that invoked sudo, when available."""
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


def transfer_result_ownership(run_dir: Path, owner: tuple[int, int]) -> None:
    """Give a completed result tree to the user who invoked sudo.

    Symlinks are deliberately not followed: benchmark artifacts must not be
    able to change ownership outside the run directory.
    """
    if not run_dir.exists():
        return
    uid, gid = owner
    for path in (run_dir, *run_dir.rglob("*")):
        os.chown(path, uid, gid, follow_symlinks=False)


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
        publication_eligible: bool,
    ):
        self.repository = repository
        self.run_dir = run_dir
        self.config = config
        self.runner = runner
        self.device = device
        self.stable_device = stable_device
        self.device_info = device_info
        self.run_id = run_id
        self.publication_eligible = publication_eligible
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
        )

    def filesystem_version(self, adapter: FilesystemAdapter) -> str:
        """Queried once per filesystem; the tools do not change mid-run."""
        if adapter.name not in self.versions:
            self.versions[adapter.name] = adapter.version()
        return self.versions[adapter.name]

    def cleanup_current(self) -> None:
        if self.current_adapter is None:
            return
        try:
            self.current_adapter.unmount()
        finally:
            self.current_adapter = None

    @contextmanager
    def fresh_filesystem(self, filesystem: str) -> Iterator[FilesystemAdapter]:
        self.cleanup_current()
        adapter = create_adapter(filesystem, self.context_for(filesystem))
        self.current_adapter = adapter
        conditioner = DeviceConditioner(
            self.runner,
            self.device,
            self.device_info,
            self.config.discard,
        )
        try:
            conditioner.condition()
            adapter.format()
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
        *,
        profile: str | None = None,
        results: Path | None = None,
        eligible: bool | None = None,
    ) -> list[str]:
        binary = (
            self.repository / "bench/bin/bench-old"
            if adapter.name == "old-bsfs"
            else self.repository / "bench/bin/bench-posix"
        )
        target_flag, target_value = adapter.target_argument
        argv = [
            str(binary),
            target_flag,
            target_value,
            "--scenario",
            scenario,
            "--phase",
            phase,
            "--profile",
            profile or self.config.profile,
            "--seed",
            str(seed),
            "--repetition",
            str(repetition),
            "--workers",
            str(workers),
            "--results",
            str(results or self.results),
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
        is_eligible = (
            self.publication_eligible if eligible is None else eligible
        )
        if is_eligible:
            argv.append("--publication-eligible")
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
        **kwargs: object,
    ) -> None:
        self.runner.run(
            self._runner_argv(
                adapter,
                scenario,
                phase,
                repetition,
                seed,
                workers,
                **kwargs,
            ),
            cwd=self.repository,
            label=(
                f"{adapter.name}-{scenario}-{phase}-r{repetition}-w{workers}"
            ),
        )

    def execute_common_scenario(
        self,
        adapter: FilesystemAdapter,
        scenario: str,
        repetition: int,
        seed: int,
        **phase_kwargs: object,
    ) -> None:
        if scenario == "bulk":
            self.run_phase(
                adapter, scenario, "prepare", repetition, seed, **phase_kwargs
            )
            cold_reset(adapter)
            self.run_phase(
                adapter, scenario, "cold-read", repetition, seed, **phase_kwargs
            )
            self.run_phase(
                adapter, scenario, "warm-read", repetition, seed, **phase_kwargs
            )
            cold_reset(adapter)
            self.run_phase(
                adapter, scenario, "random-read", repetition, seed, **phase_kwargs
            )
            self.run_phase(
                adapter, scenario, "random-write", repetition, seed, **phase_kwargs
            )
            reopen(adapter)
            self.run_phase(
                adapter, scenario, "verify", repetition, seed, **phase_kwargs
            )
            return
        if scenario == "metadata":
            self.run_phase(
                adapter, scenario, "create", repetition, seed, **phase_kwargs
            )
            # lookups and iteration hit the dentry cache unless it is dropped,
            # which would make every kernel filesystem look identical
            cold_reset(adapter)
            self.run_phase(
                adapter, scenario, "stat-cold", repetition, seed, **phase_kwargs
            )
            self.run_phase(
                adapter, scenario, "stat-warm", repetition, seed, **phase_kwargs
            )
            cold_reset(adapter)
            for phase in ("iterate", "rename", "unlink", "rmdir"):
                self.run_phase(
                    adapter, scenario, phase, repetition, seed, **phase_kwargs
                )
            reopen(adapter)
            self.run_phase(
                adapter, scenario, "verify", repetition, seed, **phase_kwargs
            )
            return
        if scenario == "mixed":
            self.run_phase(
                adapter, scenario, "create", repetition, seed, **phase_kwargs
            )
            cold_reset(adapter)
            self.run_phase(
                adapter, scenario, "read", repetition, seed, **phase_kwargs
            )
            self.run_phase(
                adapter, scenario, "edit", repetition, seed, **phase_kwargs
            )
            reopen(adapter)
            self.run_phase(
                adapter, scenario, "verify", repetition, seed, **phase_kwargs
            )
            return
        raise ValueError(f"unknown common scenario: {scenario}")

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

    def validation_matrix(self, filesystems: tuple[str, ...]) -> None:
        validation_results = self.run_dir / "validation.jsonl"
        for filesystem in filesystems:
            try:
                with self.fresh_filesystem(filesystem) as adapter:
                    self.execute_common_scenario(
                        adapter,
                        "bulk",
                        0,
                        self.config.seeds[0],
                        profile="quick",
                        results=validation_results,
                        eligible=False,
                    )
            except (CommandError, FilesystemUnavailable, RuntimeError) as error:
                self.record_failure(filesystem, "validation-bulk", 0, error)
                if self.config.publication:
                    raise

    def common_matrix(self, filesystems: tuple[str, ...]) -> None:
        for repetition in range(self.config.repetitions):
            seed = self.config.seeds[repetition]
            for filesystem in cyclic_schedule(filesystems, repetition):
                for scenario in self.config.common_scenarios:
                    try:
                        with self.fresh_filesystem(filesystem) as adapter:
                            self.execute_common_scenario(
                                adapter, scenario, repetition, seed
                            )
                    except (
                        CommandError,
                        FilesystemUnavailable,
                        RuntimeError,
                    ) as error:
                        self.record_failure(
                            filesystem, scenario, repetition, error
                        )

    def recovery_matrix(self, filesystems: tuple[str, ...]) -> None:
        for repetition in range(self.config.repetitions):
            seed = self.config.seeds[repetition]
            if "bsfs" in filesystems:
                try:
                    with self.fresh_filesystem("bsfs") as adapter:
                        self.run_phase(
                            adapter,
                            "recovery-pressure",
                            "run",
                            repetition,
                            seed,
                        )
                        reopen(adapter)
                        self.run_phase(
                            adapter,
                            "recovery-pressure",
                            "verify",
                            repetition,
                            seed,
                        )
                except (
                    CommandError,
                    FilesystemUnavailable,
                    RuntimeError,
                ) as error:
                    self.record_failure(
                        "bsfs", "recovery-pressure", repetition, error
                    )
            if "old-bsfs" in filesystems:
                try:
                    with self.fresh_filesystem("old-bsfs") as adapter:
                        self.run_phase(
                            adapter, "old-recovery", "prepare", repetition, seed
                        )
                        reopen(adapter)
                        self.run_phase(
                            adapter,
                            "old-recovery",
                            "retention",
                            repetition,
                            seed,
                        )
                    with self.fresh_filesystem("old-bsfs") as adapter:
                        self.run_phase(
                            adapter, "old-recovery", "prepare", repetition, seed
                        )
                        self.run_phase(
                            adapter, "old-recovery", "restore", repetition, seed
                        )
                        self.run_phase(
                            adapter, "old-recovery", "purge", repetition, seed
                        )
                except (
                    CommandError,
                    FilesystemUnavailable,
                    RuntimeError,
                ) as error:
                    self.record_failure(
                        "old-bsfs", "old-recovery", repetition, error
                    )

    def concurrency_matrix(self, filesystems: tuple[str, ...]) -> None:
        kernel_filesystems = tuple(
            filesystem
            for filesystem in filesystems
            if filesystem in KERNEL_FILESYSTEMS
        )
        for repetition in range(self.config.repetitions):
            seed = self.config.seeds[repetition]
            for filesystem in cyclic_schedule(kernel_filesystems, repetition):
                for scenario in self.config.concurrency_scenarios:
                    for workers in (1, 4):
                        if len(self.config.concurrency_cpus) < workers:
                            self.record_failure(
                                filesystem,
                                scenario,
                                repetition,
                                RuntimeError(
                                    f"{workers} configured CPUs are required"
                                ),
                            )
                            continue
                        try:
                            with self.fresh_filesystem(filesystem) as adapter:
                                if scenario == "concurrency-seq":
                                    self.run_phase(
                                        adapter,
                                        scenario,
                                        "write",
                                        repetition,
                                        seed,
                                        workers,
                                    )
                                    cold_reset(adapter)
                                    self.run_phase(
                                        adapter,
                                        scenario,
                                        "read",
                                        repetition,
                                        seed,
                                        workers,
                                    )
                                elif scenario == "concurrency-mixed":
                                    self.run_phase(
                                        adapter,
                                        scenario,
                                        "create",
                                        repetition,
                                        seed,
                                        workers,
                                    )
                                    cold_reset(adapter)
                                    self.run_phase(
                                        adapter,
                                        scenario,
                                        "read",
                                        repetition,
                                        seed,
                                        workers,
                                    )
                                    self.run_phase(
                                        adapter,
                                        scenario,
                                        "edit",
                                        repetition,
                                        seed,
                                        workers,
                                    )
                                    reopen(adapter)
                                    self.run_phase(
                                        adapter,
                                        scenario,
                                        "verify",
                                        repetition,
                                        seed,
                                        workers,
                                    )
                                else:
                                    raise ValueError(
                                        f"unknown concurrency scenario: {scenario}"
                                    )
                        except (
                            CommandError,
                            FilesystemUnavailable,
                            RuntimeError,
                        ) as error:
                            self.record_failure(
                                filesystem, scenario, repetition, error
                            )


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
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-correctness-gates", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repository = Path(__file__).resolve().parents[2]
    old_repository = repository.parent / "bsfs_old"
    config = load_config(args.config.resolve())
    selected = tuple(args.filesystems or config.filesystems)
    unknown = set(selected) - set(COMMON_FILESYSTEMS)
    if unknown:
        raise SystemExit(f"unknown filesystems: {', '.join(sorted(unknown))}")
    if not args.dry_run:
        try:
            validate_benchmark_binaries(repository, selected)
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
    runner = CommandRunner(run_dir, dry_run=args.dry_run)
    result_owner = sudo_invoker_owner()
    device_info: DeviceInfo | None = None
    loop_device: str | None = None
    publication_eligible = config.publication and args.device is not None

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
            print(
                "benchmark target: "
                f"path={device_info.requested} resolved={device_info.resolved} "
                f"parent={device_info.parent} model={device_info.model!r} "
                f"serial={device_info.serial!r} "
                f"size_bytes={device_info.size_bytes}"
            )
        else:
            assert args.loop_image is not None
            validate_loop_image(args.loop_image, args.confirm)
            publication_eligible = False
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
            print(
                "benchmark target: "
                f"loop_image={args.loop_image} size_bytes="
                f"{config.loop_size_mib * 1024**2}"
            )

        runner.run(["make", "-C", str(repository)], label="build-current-bsfs")
        debugfs = runner.run(
            ["mountpoint", "--quiet", "/sys/kernel/debug"],
            check=False,
            label="check-debugfs",
        )
        if debugfs.returncode != 0:
            runner.run(
                ["mount", "-t", "debugfs", "debugfs", "/sys/kernel/debug"],
                label="mount-debugfs",
            )
        kernel_before = runner.run(
            ["dmesg", "--color=never"],
            check=config.publication,
            label="kernel-log-before",
        )

        correctness: list[dict[str, object]] = []
        if config.publication and not args.skip_correctness_gates:
            try:
                correctness = run_correctness_gates(runner, repository)
            except CorrectnessGateError as error:
                gate_failure = run_dir / "correctness-gates.json"
                if not args.dry_run:
                    gate_failure.parent.mkdir(parents=True, exist_ok=True)
                    gate_failure.write_text(
                        json.dumps(error.outcomes, indent=2) + "\n",
                        encoding="utf-8",
                    )
                raise
        elif args.skip_correctness_gates:
            publication_eligible = False

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
            publication_eligible=publication_eligible,
        )
        manifest = collect_manifest(
            repository=repository,
            old_repository=old_repository,
            config_path=args.config.resolve(),
            config=config,
            run_id=run_id,
            device=manifest_device,
            affinity=(config.common_cpu, *config.concurrency_cpus),
            publication_eligible=publication_eligible,
        )
        manifest["correctness_gates"] = correctness
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
        # generated from the adapters so it cannot drift from what is actually run
        filesystem_configuration: dict[str, object] = {}
        for filesystem in selected:
            try:
                adapter = create_adapter(
                    filesystem, benchmark.context_for(filesystem)
                )
            except (FilesystemUnavailable, ValueError) as error:
                filesystem_configuration[filesystem] = {"unavailable": str(error)}
                continue
            filesystem_configuration[filesystem] = {
                **adapter.configuration(),
                "version": benchmark.filesystem_version(adapter),
            }
        manifest["filesystem_configuration"] = filesystem_configuration
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
            with PerformanceGovernor(runner, cpus):
                if config.publication:
                    benchmark.validation_matrix(selected)
                benchmark.common_matrix(selected)
                benchmark.recovery_matrix(selected)
                benchmark.concurrency_matrix(selected)
        finally:
            benchmark.cleanup_current()
            for sig, previous in previous_handlers.items():
                signal.signal(sig, previous)

        manifest["failures"] = benchmark.failures
        manifest["completed_at"] = datetime.now(timezone.utc).isoformat()
        if benchmark.failures:
            manifest["publication_eligible"] = False
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
            manifest["publication_eligible"] = False
        if config.publication and kernel_after.returncode != 0:
            benchmark.record_failure(
                "host",
                "kernel-log-capture",
                config.repetitions - 1,
                RuntimeError("could not capture the post-run kernel log"),
            )
            manifest["publication_eligible"] = False
        if not args.dry_run:
            kernel_log = run_dir / "raw" / "kernel.log"
            kernel_log.parent.mkdir(parents=True, exist_ok=True)
            kernel_log.write_text(excerpt, encoding="utf-8")
            write_manifest(run_dir, manifest)
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
        return 130
    finally:
        if loop_device:
            runner.run(
                ["losetup", "--detach", loop_device],
                check=False,
                label="detach-loop-device",
            )
        if result_owner:
            try:
                transfer_result_ownership(run_dir, result_owner)
            except OSError as error:
                print(
                    f"benchmark result ownership transfer failed: {error}",
                    file=sys.stderr,
                )


if __name__ == "__main__":
    raise SystemExit(main())
