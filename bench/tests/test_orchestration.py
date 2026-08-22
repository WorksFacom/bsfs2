from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any
import unittest
from unittest.mock import call, patch

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY))

from bench.orchestration.analyze import (
    derive_scaling,
    derive_summary,
    load_records,
)
from bench.orchestration.report import render_report
from bench.orchestration.command import CommandResult, CommandRunner
from bench.orchestration.config import load_config
from bench.orchestration.preflight import (
    BenchmarkBinaryError,
    validate_benchmark_binaries,
)
from bench.orchestration.modules import (
    BsfsModuleError,
    alloc_strategies,
    build_module_argv,
    build_modules,
    module_path,
)
from bench.orchestration.safety import (
    SafetyError,
    cyclic_schedule,
    validate_loop_image,
)
from bench.orchestration.topology import physical_cores, sibling_conflicts
from bench.orchestration.filesystems import (
    ADAPTERS,
    BSFS_FILESYSTEMS,
    FilesystemAdapter,
    FilesystemContext,
    bsfs_targets_by_strategy,
    create_adapter,
)
from bench.orchestration.plan import build_plan
from bench.orchestration.progress import (
    Level,
    PhaseMetricsReader,
    RunReporter,
    describe_metrics,
    format_duration,
)
from bench.orchestration.run import (
    BenchmarkRun,
    main,
    sudo_invoker_owner,
    transfer_result_ownership,
)

BUILD_ARGV = build_module_argv(REPOSITORY)


class ConfigTests(unittest.TestCase):
    def test_quick_profile_is_valid(self) -> None:
        config = load_config(REPOSITORY / "bench/config/quick.toml")

        self.assertEqual(config.profile, "quick")
        self.assertEqual(config.seeds, (1234,))
        self.assertIn("old-bsfs", config.filesystems)
        self.assertGreaterEqual(len(config.concurrency_cpus), 4)

    def test_worker_cpus_land_on_distinct_physical_cores(self) -> None:
        config = load_config(REPOSITORY / "bench/config/publication.toml")

        self.assertEqual(sibling_conflicts(config.concurrency_cpus), [])
        self.assertNotIn(config.common_cpu, config.concurrency_cpus)

    def test_sibling_cpu_lists_are_rejected(self) -> None:
        cores = physical_cores()
        smt = next((core for core in cores if len(core) > 1), None)
        if smt is None:
            self.skipTest("host has no SMT siblings to conflict")

        self.assertNotEqual(sibling_conflicts(tuple(smt[:2])), [])


class SafetyTests(unittest.TestCase):
    def test_cyclic_schedule_rotates_without_losing_items(self) -> None:
        items = ("bsfs", "ext4", "btrfs")

        self.assertEqual(cyclic_schedule(items, 0), items)
        self.assertEqual(
            cyclic_schedule(items, 1), ("ext4", "btrfs", "bsfs")
        )
        self.assertEqual(cyclic_schedule(items, 4), ("ext4", "btrfs", "bsfs"))

    def test_new_loop_image_needs_no_confirmation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            validate_loop_image(Path(directory) / "new.img", None)

    def test_existing_loop_image_requires_exact_resolved_confirmation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "existing.img"
            image.touch()

            with self.assertRaises(SafetyError):
                validate_loop_image(image, None)
            validate_loop_image(image, str(image.resolve()))

    def test_loop_image_symlink_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "target.img"
            link = Path(directory) / "link.img"
            target.touch()
            link.symlink_to(target)

            with self.assertRaises(SafetyError):
                validate_loop_image(link, str(link.resolve()))


class CommandTests(unittest.TestCase):
    def test_dry_run_does_not_create_output_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            result = CommandRunner(run_dir, dry_run=True).run(
                ["definitely-not-a-command"], label="dry"
            )

            self.assertEqual(result.returncode, 0)
            self.assertFalse(run_dir.exists())


class ResultOwnershipTests(unittest.TestCase):
    def test_sudo_invoker_owner_uses_sudo_identity(self) -> None:
        self.assertEqual(
            sudo_invoker_owner({"SUDO_UID": "1000", "SUDO_GID": "1001"}),
            (1000, 1001),
        )

    def test_sudo_invoker_owner_ignores_missing_or_invalid_identity(self) -> None:
        self.assertIsNone(sudo_invoker_owner({}))
        self.assertIsNone(
            sudo_invoker_owner({"SUDO_UID": "not-a-number", "SUDO_GID": "1"})
        )
        self.assertIsNone(
            sudo_invoker_owner({"SUDO_UID": "1", "SUDO_GID": "-1"})
        )

    def test_transfer_result_ownership_visits_every_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            nested = run_dir / "raw" / "artifacts"
            nested.mkdir(parents=True)
            result = nested / "results.jsonl"
            result.touch()

            with patch("bench.orchestration.run.os.chown") as chown:
                transfer_result_ownership(run_dir, (1000, 1001))

            self.assertCountEqual(
                chown.call_args_list,
                [
                    call(path, 1000, 1001, follow_symlinks=False)
                    for path in (run_dir, run_dir / "raw", nested, result)
                ],
            )

    def test_transfer_result_ownership_skips_a_mounted_mountpoint(self) -> None:
        """a failed umount leaves a live benchmark filesystem under run_dir,
        and chowning through it would rewrite the device under test."""
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            mounted = run_dir / "mnt"
            (mounted / "payload").mkdir(parents=True)
            (mounted / "payload" / "big.bin").touch()
            results = run_dir / "raw"
            results.mkdir()
            (results / "results.jsonl").touch()

            with (
                patch("bench.orchestration.run.os.chown") as chown,
                patch(
                    "bench.orchestration.run.os.path.ismount",
                    side_effect=lambda path: Path(path) == mounted,
                ),
            ):
                self.assertEqual(
                    transfer_result_ownership(run_dir, (1000, 1001)), []
                )

            touched = {arguments[0] for arguments, _ in chown.call_args_list}
            self.assertEqual(
                touched, {run_dir, results, results / "results.jsonl"}
            )
            self.assertNotIn(mounted, touched)

    def test_transfer_result_ownership_continues_past_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            run_dir.mkdir()
            stubborn = run_dir / "stubborn"
            stubborn.touch()
            (run_dir / "results.jsonl").touch()

            def chown(path: Path, *_: object, **__: object) -> None:
                if path == stubborn:
                    raise PermissionError("read-only file system")

            with patch("bench.orchestration.run.os.chown", side_effect=chown):
                failed = transfer_result_ownership(run_dir, (1000, 1001))

            self.assertEqual(len(failed), 1)
            self.assertIn("stubborn", failed[0])


class BenchmarkBinaryTests(unittest.TestCase):
    def create_binary(self, repository: Path, name: str) -> Path:
        binary = repository / "bench/bin" / name
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.touch(mode=0o755)
        return binary

    def test_posix_selection_requires_only_posix_runner(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            self.create_binary(repository, "bench-posix")

            validate_benchmark_binaries(repository, ("bsfs", "ext4"))

    def test_old_bsfs_selection_requires_its_runner_and_formatter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            self.create_binary(repository, "bench-old")

            with self.assertRaisesRegex(
                BenchmarkBinaryError,
                "make -C bench OLD_BSFS_DIR=/absolute/path/to/bsfs_old",
            ):
                validate_benchmark_binaries(repository, ("old-bsfs",))

    def test_non_executable_runner_is_rejected_with_posix_build_command(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            binary = self.create_binary(repository, "bench-posix")
            binary.chmod(0o644)

            with self.assertRaisesRegex(
                BenchmarkBinaryError, "make -C bench bin/bench-posix"
            ):
                validate_benchmark_binaries(repository, ("ext4",))


class BsfsModulePreflightTests(unittest.TestCase):
    def build_argv(self, strategy: str) -> list[str]:
        return build_module_argv(REPOSITORY, strategy)

    def patched(self, *, loaded: bool = False, built_as_ok: bool = True):
        """Patch writes to bench/build and strategy verification."""
        assertion = (
            patch("bench.orchestration.modules._assert_built_as")
            if built_as_ok
            else patch(
                "bench.orchestration.modules._assert_built_as",
                side_effect=BsfsModuleError("wrong strategy"),
            )
        )
        return (
            patch("bench.orchestration.modules._run_capture"),
            patch("bench.orchestration.modules.shutil.copy2"),
            patch(
                "bench.orchestration.modules._module_loaded", return_value=loaded
            ),
            assertion,
        )

    def test_each_strategy_is_built_stashed_loaded_and_unloaded(self) -> None:
        stash = module_path(REPOSITORY, "heap")
        capture, copy, _loaded, _built = self.patched()
        with capture as run, copy as copied, _loaded, _built:
            run.side_effect = [
                subprocess.CompletedProcess(self.build_argv("heap"), 0, "", ""),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 1, "", ""),
                subprocess.CompletedProcess(["insmod", str(stash)], 0, "", ""),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 0, "", ""),
                subprocess.CompletedProcess(BUILD_ARGV, 0, "", ""),
            ]

            stashed = build_modules(REPOSITORY, ("heap",))

        self.assertEqual(stashed, (stash,))
        self.assertEqual(
            run.call_args_list,
            [
                call(self.build_argv("heap")),
                call(["rmmod", "bsfs2"]),
                call(["insmod", str(stash)]),
                call(["rmmod", "bsfs2"]),
                # Correctness gates and QEMU scripts boot the default build.
                call(BUILD_ARGV),
            ],
        )
        copied.assert_called_once_with(REPOSITORY / "bsfs2.ko", stash)

    def test_strategy_build_pins_the_allocator_and_clears_the_counters(
        self,
    ) -> None:
        argv = self.build_argv("twolevel")

        self.assertIn("BSFS_ALLOC=twolevel", argv)
        # Allocation counters change the measured fast path. A .env setting
        # must not affect a benchmark build.
        self.assertIn("BSFS_ALLOC_STATS=", argv)

    def test_build_pins_pwd_to_the_repository(self) -> None:
        """make -C moves CURDIR but not PWD, and the kernel build uses
        M=$(PWD), so a launcher started elsewhere would build the wrong tree."""
        self.assertIn(f"PWD={REPOSITORY}", BUILD_ARGV)
        self.assertEqual(BUILD_ARGV[:3], ["make", "-C", str(REPOSITORY)])

    def test_unknown_strategy_is_refused_before_anything_is_built(self) -> None:
        capture, copy, _loaded, _built = self.patched()
        with capture as run, copy, _loaded, _built:
            with self.assertRaisesRegex(BsfsModuleError, "unknown allocation"):
                build_modules(REPOSITORY, ("nonesuch",))

        run.assert_not_called()

    def test_a_build_that_did_not_take_is_not_stashed(self) -> None:
        capture, copy, _loaded, _built = self.patched(built_as_ok=False)
        with capture as run, copy as copied, _loaded, _built:
            run.side_effect = [
                subprocess.CompletedProcess(self.build_argv("heap"), 0, "", "")
            ]

            with self.assertRaises(BsfsModuleError):
                build_modules(REPOSITORY, ("heap",))

        copied.assert_not_called()

    def test_recorded_defines_must_match_the_requested_strategy(self) -> None:
        from bench.orchestration.modules import _assert_built_as

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            command_file = repository / ".bsfs.o.cmd"

            command_file.write_text(
                'cmd_bsfs.o := gcc -DBSFS_ALLOC_STRATEGY_NAME=\\"linear\\" -c\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(BsfsModuleError, "not the requested build"):
                _assert_built_as(repository, "heap")

            command_file.write_text(
                'cmd_bsfs.o := gcc -DBSFS_ALLOC_STRATEGY_NAME=\\"heap\\" '
                "-DBSFS_ALLOC_STATS -c\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(BsfsModuleError, "BSFS_ALLOC_STATS"):
                _assert_built_as(repository, "heap")

            command_file.write_text(
                'cmd_bsfs.o := gcc -DBSFS_ALLOC_STRATEGY_NAME=\\"heap\\" -c\n',
                encoding="utf-8",
            )
            _assert_built_as(repository, "heap")

    def test_load_failure_reports_kernel_and_vermagic(self) -> None:
        stash = module_path(REPOSITORY, "linear")
        capture, copy, _loaded, _built = self.patched()
        with capture as run, copy, _loaded, _built:
            run.side_effect = [
                subprocess.CompletedProcess(self.build_argv("linear"), 0, "", ""),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 1, "", ""),
                subprocess.CompletedProcess(
                    ["insmod", str(stash)], 1, "", "Invalid module format"
                ),
                subprocess.CompletedProcess(
                    ["modinfo", "-F", "vermagic", str(stash)],
                    0,
                    "6.18.15-dirty SMP preempt mod_unload\n",
                    "",
                ),
            ]

            with self.assertRaises(BsfsModuleError) as raised:
                build_modules(REPOSITORY, ("linear",))

        message = str(raised.exception)
        self.assertIn("Invalid module format", message)
        self.assertIn("module vermagic: 6.18.15-dirty", message)
        self.assertIn("KDIR", message)

    def test_busy_module_is_not_blamed_on_kdir(self) -> None:
        stash = module_path(REPOSITORY, "linear")
        capture, copy, _loaded, _built = self.patched(loaded=True)
        with capture as run, copy, _loaded, _built:
            run.side_effect = [
                subprocess.CompletedProcess(self.build_argv("linear"), 0, "", ""),
                subprocess.CompletedProcess(
                    ["rmmod", "bsfs2"], 1, "", "rmmod: ERROR: Module bsfs2 is in use"
                ),
                subprocess.CompletedProcess(
                    ["modinfo", "-F", "vermagic", str(stash)], 0, "6.18.15\n", ""
                ),
            ]

            with self.assertRaises(BsfsModuleError) as raised:
                build_modules(REPOSITORY, ("linear",))

        message = str(raised.exception)
        self.assertIn("still in use", message)
        self.assertNotIn("KDIR", message)
        # insmod must not run against a module that is already loaded and busy
        self.assertNotIn(call(["insmod", str(stash)]), run.call_args_list)


class AllocStrategyTargetTests(unittest.TestCase):
    def test_every_strategy_source_has_a_benchmark_target(self) -> None:
        for strategy in alloc_strategies(REPOSITORY):
            self.assertIn(f"bsfs-{strategy}", ADAPTERS)
            self.assertEqual(
                ADAPTERS[f"bsfs-{strategy}"].alloc_strategy, strategy
            )
        self.assertEqual(
            BSFS_FILESYSTEMS,
            ("bsfs", *(f"bsfs-{s}" for s in alloc_strategies(REPOSITORY))),
        )
        # old-bsfs is separate from the BSFS allocator variants.
        self.assertNotIn("old-bsfs", BSFS_FILESYSTEMS)

    def test_plain_bsfs_still_means_the_default_build(self) -> None:
        self.assertEqual(ADAPTERS["bsfs"].alloc_strategy, "linear")

    def test_a_variant_loads_its_own_stashed_module(self) -> None:
        recorded: list[tuple[str, ...]] = []

        class Recorder:
            def dry_command(self, argv: tuple[str, ...]) -> None:
                recorded.append(argv)

        runner = CommandRunner(
            REPOSITORY / "bench/results/unused",
            dry_run=True,
            reporter=Recorder(),
        )
        adapter = create_adapter(
            "bsfs-heap",
            FilesystemContext(
                runner=runner,
                repository=REPOSITORY,
                device="/dev/loop-dry-run",
                stable_device="/dev/loop-dry-run",
                mountpoint=Path("/mnt/bsfs-bench"),
                run_id="test",
            ),
        )

        adapter.mount()

        insmod = [argv for argv in recorded if argv[0] == "insmod"]
        self.assertEqual(
            insmod, [("insmod", str(module_path(REPOSITORY, "heap")))]
        )
        self.assertEqual(adapter.configuration()["alloc_strategy"], "heap")

    def test_shared_builds_are_collapsed_into_one_compile(self) -> None:
        # "bsfs" and "bsfs-linear" name the same module.
        self.assertEqual(
            bsfs_targets_by_strategy(
                ("bsfs", "bsfs-linear", "bsfs-heap", "ext4", "old-bsfs")
            ),
            {"linear": ("bsfs", "bsfs-linear"), "heap": ("bsfs-heap",)},
        )


class AnalyzeEntryPointTests(unittest.TestCase):
    def test_analyze_runs_as_a_script_from_an_unrelated_directory(self) -> None:
        """analyze.py is executable and has a shebang, so it needs to work
        without the repository root already on sys.path."""
        script = REPOSITORY / "bench/orchestration/analyze.py"
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run"
            raw = run_dir / "raw"
            raw.mkdir(parents=True)
            (raw / "results.jsonl").write_text(
                json.dumps(result(workers=1, throughput=10.0)) + "\n",
                encoding="utf-8",
            )

            completed = subprocess.run(
                [sys.executable, str(script), str(run_dir)],
                cwd=directory,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = run_dir / "derived" / "report.html"
            self.assertTrue(report.is_file())
            self.assertIn("BSFS benchmark report", report.read_text("utf-8"))


class BsfsSetupFailureTests(unittest.TestCase):
    def test_module_preflight_failure_creates_no_run_directory(self) -> None:
        config = REPOSITORY / "bench/config/quick.toml"
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "results"
            loop_image = Path(directory) / "bench.img"
            args = argparse.Namespace(
                config=config,
                device=None,
                loop_image=loop_image,
                confirm=None,
                allow_size_outside_range=False,
                allow_shared_disk=False,
                run_dir=run_dir,
                filesystems=("bsfs",),
                dry_run=False,
                skip_correctness_gates=False,
                verbose=False,
                quiet=True,
                no_color=True,
            )
            with (
                patch("bench.orchestration.run.parse_args", return_value=args),
                patch("bench.orchestration.run.os.geteuid", return_value=0),
                patch("bench.orchestration.run.validate_benchmark_binaries"),
                patch(
                    "bench.orchestration.run.build_modules",
                    side_effect=BsfsModuleError("invalid module format"),
                ),
                patch("bench.orchestration.run.CommandRunner") as runner,
            ):
                self.assertEqual(main(), 2)

            self.assertFalse(run_dir.exists())
            self.assertFalse(loop_image.exists())
            runner.assert_not_called()

    def test_non_bsfs_selection_skips_module_preflight(self) -> None:
        config = REPOSITORY / "bench/config/quick.toml"
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(
                config=config,
                device="/dev/benchmark-test",
                loop_image=None,
                confirm="/dev/benchmark-test",
                allow_size_outside_range=False,
                allow_shared_disk=False,
                run_dir=Path(directory) / "results",
                filesystems=("ext4",),
                dry_run=False,
                skip_correctness_gates=False,
                verbose=False,
                quiet=True,
                no_color=True,
            )
            with (
                patch("bench.orchestration.run.parse_args", return_value=args),
                patch("bench.orchestration.run.os.geteuid", return_value=0),
                patch("bench.orchestration.run.validate_benchmark_binaries"),
                patch("bench.orchestration.run.build_modules") as module,
                patch("bench.orchestration.run.CommandRunner"),
                patch(
                    "bench.orchestration.run.inspect_physical_device",
                    side_effect=SafetyError("stop after preflight"),
                ),
            ):
                self.assertEqual(main(), 2)

            module.assert_not_called()


def result(
    *,
    workers: int,
    throughput: float,
    worker_id: int | None = None,
    sectors_written: int | None = None,
    durable_window: bool = True,
) -> dict[str, object]:
    return {
        "filesystem": "bsfs",
        "scenario": "concurrency-seq",
        "phase": "write",
        "worker_count": workers,
        "worker_id": worker_id,
        "validation_status": "passed",
        "throughput_mib_s": throughput * 4,
        "durable_throughput_mib_s": throughput,
        "logical_bytes": 4096,
        "device_sectors_written": sectors_written,
        "has_durable_window": durable_window,
    }


def embedded_report_data(report: str) -> dict[str, Any]:
    return json.loads(
        report.split('id="report-data" type="application/json">')[1]
        .split("</script>")[0]
        .replace("\\u003c", "<")
        .replace("\\u003e", ">")
        .replace("\\u0026", "&")
    )


class AnalysisTests(unittest.TestCase):
    def test_report_is_self_contained_and_preserves_complete_data(self) -> None:
        row = derive_summary([result(workers=1, throughput=10.0)])[0]
        row["phase"] = "write </script><script>alert(1)</script>"
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=1,
            summary=[row],
            scaling=[
                {
                    "filesystem": "bsfs",
                    "scenario": "concurrency-seq",
                    "phase": "write",
                    "one_worker_mib_s": 10.0,
                    "four_worker_mib_s": 30.0,
                    "scaling_ratio": 3.0,
                }
            ],
            failures=[{"filesystem": "bsfs", "error": "bad <module>"}],
            skipped=["results.jsonl:2: bad <record>"],
            manifest={
                "run_id": "run-1",
                "publication_eligible": False,
                "hostname": "test-host",
                "config": {"profile": "quick", "repetitions": 1},
                "kernel": {"release": "test-kernel"},
                "filesystem_schedule": [
                    {"repetition": 0, "order": ["bsfs", "ext4"]}
                ],
            },
        )

        self.assertIn("BSFS benchmark report — run-1", report)
        self.assertIn("Not publication eligible", report)
        self.assertIn("Complete derived data", report)
        self.assertIn("durable_throughput_mib_s_median", report)
        self.assertIn("scaling_ratio", report)
        self.assertIn("test-host", report)
        self.assertIn("bsfs &rarr; ext4", report)
        self.assertIn("\\u003c/script", report)
        self.assertNotIn("<script>alert(1)</script>", report)
        self.assertNotIn("<script src=", report)
        self.assertNotIn("<link ", report)
        self.assertNotIn("<img ", report)

    def test_report_has_page_local_light_and_dark_theme_switch(self) -> None:
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[],
            skipped=[],
            manifest={},
        )

        self.assertIn('role="switch" aria-checked="false"', report)
        self.assertIn('aria-label="Switch to dark mode"', report)
        self.assertIn('html[data-theme="dark"]', report)
        self.assertIn('setTheme("light");', report)
        self.assertIn('themeSwitch.addEventListener("click"', report)
        self.assertNotIn("localStorage", report)

    def test_empty_report_still_renders_without_manifest(self) -> None:
        report = render_report(
            run_dir=Path("/results/interrupted-run"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[],
            skipped=[],
            manifest={},
        )

        self.assertIn("interrupted-run", report)
        self.assertIn("Eligibility not recorded", report)
        self.assertIn('"summary":[]', report)

    def test_a_manifest_value_cannot_expand_into_another_marker(self) -> None:
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[],
            skipped=[],
            manifest={"run_id": "run-1", "hostname": "__REPORT_DATA__"},
        )

        self.assertIn("<dd>__REPORT_DATA__</dd>", report)
        self.assertEqual(report.count('"summary":[]'), 1)

    def test_a_non_finite_metric_fails_loudly_instead_of_blanking_the_page(
        self,
    ) -> None:
        """a bare NaN token makes JSON.parse throw, which would leave the
        reader an empty page instead of just a broken number."""
        with self.assertRaises(ValueError):
            render_report(
                run_dir=Path("/results/run-1"),
                record_count=1,
                summary=[{"filesystem": "bsfs", "durable_iops_median": float("nan")}],
                scaling=[],
                failures=[],
                skipped=[],
                manifest={},
            )

    def test_failures_are_attributed_to_the_chart_they_are_missing_from(
        self,
    ) -> None:
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[
                {"filesystem": "btrfs", "scenario": "bulk", "phase": "verify"},
                {"filesystem": "zfs", "scenario": "bulk", "error": "no zpool"},
                {"error": "kernel log finding"},
            ],
            skipped=[],
            manifest={},
        )

        payload = embedded_report_data(report)

        # a scenario-wide failure has no phase, so it belongs to every phase
        self.assertEqual(
            payload["failedByPhase"],
            {"bulk": {"": ["zfs"], "verify": ["btrfs"]}},
        )

    def test_report_describes_known_generated_and_unknown_chart_phases(
        self,
    ) -> None:
        phases = (
            ("bulk", "sequential-write", None),
            ("metadata", "lookup-stat-warm", None),
            ("mixed", "edit-cycle-durable-total", None),
            ("concurrency-mixed", "mixed-create", None),
            ("concurrency-mixed", "mixed-create-worker-1", 1),
            ("recovery-pressure", "cycle-2-refill", None),
            (
                "recovery-pressure",
                "cycle-1-native-namespace-removal",
                None,
            ),
            ("old-recovery", "restore-fixed-subset", None),
            ("future-workload", "novel-phase", None),
        )
        summary = [
            {
                "filesystem": "bsfs",
                "scenario": scenario,
                "phase": phase,
                "worker_count": 4 if scenario.startswith("concurrency-") else 1,
                "worker_id": worker_id,
                "durable_throughput_mib_s_median": 1.0,
            }
            for scenario, phase, worker_id in phases
        ]

        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=len(summary),
            summary=summary,
            scaling=[],
            failures=[],
            skipped=[],
            manifest={},
        )
        descriptions = embedded_report_data(report)["phaseDescriptions"]

        self.assertIn("1 MiB requests", descriptions["bulk"]["sequential-write"])
        self.assertIn("dentries cached", descriptions["metadata"]["lookup-stat-warm"])
        self.assertIn("final flush", descriptions["mixed"]["edit-cycle-durable-total"])
        self.assertIn(
            "aggregates all workers",
            descriptions["concurrency-mixed"]["mixed-create"],
        )
        self.assertIn(
            "isolates worker 1",
            descriptions["concurrency-mixed"]["mixed-create-worker-1"],
        )
        self.assertIn(
            "Recovery cycle 2",
            descriptions["recovery-pressure"]["cycle-2-refill"],
        )
        self.assertIn(
            "preceding refill files",
            descriptions["recovery-pressure"]["cycle-1-native-namespace-removal"],
        )
        self.assertIn(
            "fixed subset",
            descriptions["old-recovery"]["restore-fixed-subset"],
        )
        self.assertEqual(
            descriptions["future-workload"]["novel-phase"],
            'Compares filesystems for benchmark phase "novel-phase".',
        )

    def test_chart_description_tracks_the_selected_metric(self) -> None:
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[],
            skipped=[],
            manifest={},
        )
        metrics = {
            metric[0]: metric[4] for metric in embedded_report_data(report)["metrics"]
        }

        self.assertIn("recorded final flush", metrics["durable_throughput_mib_s"])
        self.assertIn("recorded final flush", metrics["durable_iops"])
        self.assertIn("Operation-path time", metrics["durable_total_ns"])
        self.assertIn("excluding the final flush", metrics["latency_p99_ns"])
        self.assertIn(
            "only for phases with a final flush", metrics["write_amplification"]
        )
        self.assertIn('element("p", "chart-description")', report)
        self.assertIn(
            "description.textContent = `${descriptionFor(scenario, phase)} ${metric[4]}`;",
            report,
        )

    def test_null_device_stats_do_not_become_zero_amplification(self) -> None:
        summary = derive_summary([result(workers=1, throughput=10.0)])

        self.assertEqual(summary[0]["write_amplification_count"], 0)
        self.assertIsNone(summary[0]["write_amplification_median"])

    def test_scaling_uses_aggregate_rows_only(self) -> None:
        records = [
            result(workers=1, throughput=10.0),
            result(workers=4, throughput=30.0),
            result(workers=4, throughput=8.0, worker_id=0),
        ]

        scaling = derive_scaling(derive_summary(records))

        self.assertEqual(len(scaling), 1)
        self.assertEqual(scaling[0]["scaling_ratio"], 3.0)

    def test_failed_records_are_excluded_from_summary(self) -> None:
        failed = result(workers=1, throughput=10.0)
        failed["validation_status"] = "failed"

        self.assertEqual(derive_summary([failed]), [])

    def test_result_fixture_is_json_serializable(self) -> None:
        json.dumps(result(workers=1, throughput=10.0))

    def test_durable_throughput_is_summarized(self) -> None:
        summary = derive_summary(
            [
                result(workers=1, throughput=10.0),
                result(workers=1, throughput=20.0),
            ]
        )

        self.assertEqual(summary[0]["durable_throughput_mib_s_median"], 15.0)
        # the page-cache rate is kept, but it is a different number
        self.assertEqual(summary[0]["throughput_mib_s_median"], 60.0)

    def test_amplification_skips_phases_without_a_flush(self) -> None:
        durable = result(workers=1, throughput=10.0, sectors_written=16)
        buffered = result(
            workers=4, throughput=10.0, sectors_written=16, durable_window=False
        )

        rows = {row["worker_count"]: row for row in derive_summary([durable, buffered])}

        self.assertEqual(rows[1]["write_amplification_count"], 1)
        self.assertEqual(rows[1]["write_amplification_median"], 2.0)
        self.assertEqual(rows[4]["write_amplification_count"], 0)
        self.assertIsNone(rows[4]["write_amplification_median"])

    def test_truncated_final_line_is_skipped_not_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.jsonl"
            path.write_text(
                json.dumps(result(workers=1, throughput=10.0))
                + "\n"
                + '{"filesystem": "bsfs", "scen',
                encoding="utf-8",
            )

            records, skipped = load_records(path)

            self.assertEqual(len(records), 1)
            self.assertEqual(len(skipped), 1)


ANSI = re.compile(r"\x1b\[[0-9;]*m")


class StubAdapter(FilesystemAdapter):
    """stands in for a real filesystem so the matrices can be walked offline."""

    mount_options = "stub"

    def __init__(self, context: FilesystemContext, name: str):
        super().__init__(context)
        self.name = name

    def format(self) -> None:
        return

    def mount(self) -> None:
        return

    def unmount(self) -> None:
        return


class PlanTests(unittest.TestCase):
    def _executed_phases(self, config_name: str) -> tuple[list, Any]:
        config = load_config(REPOSITORY / f"bench/config/{config_name}.toml")
        executed: list[tuple[str, str, str, int]] = []

        def record(
            self, adapter, scenario, phase, repetition, seed, workers=1, **kwargs
        ):
            executed.append((adapter.name, scenario, phase, workers))

        with tempfile.TemporaryDirectory() as directory:
            reporter = RunReporter(stream=io.StringIO(), level=Level.QUIET)
            runner = CommandRunner(
                Path(directory), dry_run=True, reporter=reporter
            )
            benchmark = BenchmarkRun(
                repository=REPOSITORY,
                run_dir=Path(directory),
                config=config,
                runner=runner,
                device="/dev/loop-test",
                stable_device="/dev/loop-test",
                device_info=None,
                run_id="test",
                publication_eligible=False,
                reporter=reporter,
            )
            with (
                patch(
                    "bench.orchestration.run.create_adapter",
                    side_effect=lambda name, context: StubAdapter(context, name),
                ),
                patch.object(
                    BenchmarkRun, "run_phase", autospec=True, side_effect=record
                ),
            ):
                if config.publication:
                    benchmark.validation_matrix(config.filesystems)
                benchmark.common_matrix(config.filesystems)
                benchmark.recovery_matrix(config.filesystems)
                benchmark.concurrency_matrix(config.filesystems)
        return executed, config

    def test_quick_plan_matches_executed_phases(self) -> None:
        executed, config = self._executed_phases("quick")
        plan = build_plan(config, config.filesystems)

        self.assertEqual(len(executed), plan.total_phases)
        for filesystem in config.filesystems:
            self.assertEqual(
                sum(1 for row in executed if row[0] == filesystem),
                plan.phases_for(filesystem),
                filesystem,
            )

    def test_publication_plan_matches_executed_phases(self) -> None:
        executed, config = self._executed_phases("publication")
        plan = build_plan(config, config.filesystems)

        self.assertEqual(len(executed), plan.total_phases)

    def test_plan_names_the_phases_the_matrices_run(self) -> None:
        executed, config = self._executed_phases("quick")
        plan = build_plan(config, config.filesystems)
        planned = sorted(
            (unit.filesystem, unit.scenario, phase, unit.workers)
            for unit in plan.units
            for phase in unit.phases
        )

        self.assertEqual(sorted(executed), planned)

    def test_alloc_plan_matches_executed_phases(self) -> None:
        """Each allocator variant runs the BSFS recovery path."""
        executed, config = self._executed_phases("alloc")
        plan = build_plan(config, config.filesystems)

        self.assertEqual(sorted(executed), sorted(
            (unit.filesystem, unit.scenario, phase, unit.workers)
            for unit in plan.units
            for phase in unit.phases
        ))
        for filesystem in config.filesystems:
            self.assertIn(
                (filesystem, "recovery-pressure", "run", 1), executed, filesystem
            )

    def test_every_allocator_variant_gets_a_recovery_unit(self) -> None:
        config = load_config(REPOSITORY / "bench/config/alloc.toml")
        plan = build_plan(config, ("bsfs-heap", "ext4"))

        recovery = [unit for unit in plan.units if unit.matrix == "recovery"]
        self.assertEqual(
            [(unit.filesystem, unit.scenario) for unit in recovery],
            [("bsfs-heap", "recovery-pressure")],
        )

    def test_a_filesystem_only_in_the_config_is_still_planned(self) -> None:
        config = load_config(REPOSITORY / "bench/config/quick.toml")
        plan = build_plan(config, ("ext4",))

        self.assertEqual(plan.filesystems, ("ext4",))
        # ext4 is a kernel filesystem, so it has no recovery units
        self.assertEqual(
            {unit.matrix for unit in plan.units}, {"common", "concurrency"}
        )


class PhaseReportingTests(unittest.TestCase):
    def test_a_phase_reports_the_metric_its_runner_just_wrote(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            stream = io.StringIO()
            reporter = RunReporter(
                stream=stream, level=Level.NORMAL, color=False
            )
            runner = CommandRunner(run_dir, reporter=reporter)
            benchmark = BenchmarkRun(
                repository=REPOSITORY,
                run_dir=run_dir,
                config=load_config(REPOSITORY / "bench/config/quick.toml"),
                runner=runner,
                device="/dev/loop-test",
                stable_device="/dev/loop-test",
                device_info=None,
                run_id="test",
                publication_eligible=False,
                reporter=reporter,
            )
            adapter = StubAdapter(benchmark.context_for("ext4"), "ext4")

            def fake_run(argv, **kwargs):
                with benchmark.results.open("a", encoding="utf-8") as results:
                    results.write(
                        json.dumps(result(workers=1, throughput=312.4)) + "\n"
                    )
                return CommandResult((), 0, "", "")

            with patch.object(runner, "run", side_effect=fake_run):
                benchmark.run_phase(adapter, "bulk", "prepare", 0, 1234)

            text = stream.getvalue()
            self.assertIn("bulk/prepare", text)
            self.assertIn("312.4 MiB/s", text)

    def test_a_phase_that_writes_nothing_still_reports_its_time(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory)
            stream = io.StringIO()
            reporter = RunReporter(
                stream=stream, level=Level.NORMAL, color=False
            )
            runner = CommandRunner(run_dir, reporter=reporter)
            benchmark = BenchmarkRun(
                repository=REPOSITORY,
                run_dir=run_dir,
                config=load_config(REPOSITORY / "bench/config/quick.toml"),
                runner=runner,
                device="/dev/loop-test",
                stable_device="/dev/loop-test",
                device_info=None,
                run_id="test",
                publication_eligible=False,
                reporter=reporter,
            )
            adapter = StubAdapter(benchmark.context_for("ext4"), "ext4")

            with patch.object(
                runner, "run", return_value=CommandResult((), 0, "", "")
            ):
                benchmark.run_phase(adapter, "bulk", "prepare", 0, 1234)

            self.assertIn("bulk/prepare", stream.getvalue())
            self.assertFalse(reporter.degraded)


class ReporterTests(unittest.TestCase):
    def _reporter(self, level: Level = Level.NORMAL) -> tuple[RunReporter, Any]:
        stream = io.StringIO()
        return RunReporter(stream=stream, level=level, color=False), stream

    def test_output_has_no_escape_sequences_without_color(self) -> None:
        reporter, stream = self._reporter()
        reporter.announce_plan(
            build_plan(load_config(REPOSITORY / "bench/config/quick.toml"), ("ext4",))
        )
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        with reporter.phase(
            filesystem="ext4", scenario="bulk", phase="prepare", workers=1
        ):
            pass
        reporter.failure("ext4", "bulk", 0, RuntimeError("boom"))

        text = stream.getvalue()
        self.assertNotIn("\x1b", text)
        self.assertIn("bulk/prepare", text)
        self.assertIn("FAILED", text)
        self.assertTrue(text.endswith("\n"))

    def test_every_line_puts_its_duration_in_one_column(self) -> None:
        reporter, stream = self._reporter()
        reporter.announce_plan(
            build_plan(load_config(REPOSITORY / "bench/config/quick.toml"), ("ext4",))
        )
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        with reporter.timed("condition device", "no-discard"):
            pass
        reporter.cache_event("unmount + drop_caches + mount", 1.2)
        for scenario, phase, workers in (
            ("bulk", "prepare", 1),
            ("concurrency-mixed", "create", 4),
        ):
            with reporter.phase(
                filesystem="ext4",
                scenario=scenario,
                phase=phase,
                workers=workers,
            ):
                pass
        reporter.end_section()

        columns = []
        for line in stream.getvalue().splitlines():
            if not line.startswith("  "):
                continue
            # the duration is the first thing on a line shaped like a time
            match = re.search(r"\d+\.\ds", line)
            if match:
                columns.append(match.end())

        self.assertGreaterEqual(len(columns), 5, stream.getvalue())
        self.assertEqual(len(set(columns)), 1, stream.getvalue())

    def test_color_only_adds_escapes_around_the_same_text(self) -> None:
        plain, plain_stream = self._reporter()
        stream = io.StringIO()
        colored = RunReporter(stream=stream, level=Level.NORMAL, color=True)
        for reporter in (plain, colored):
            reporter.failure("ext4", "bulk", 0, RuntimeError("boom"))

        self.assertIn("\x1b", stream.getvalue())
        self.assertEqual(
            ANSI.sub("", stream.getvalue()), plain_stream.getvalue()
        )

    def test_quiet_keeps_failures_and_drops_phase_lines(self) -> None:
        reporter, stream = self._reporter(Level.QUIET)
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        with reporter.phase(
            filesystem="ext4", scenario="bulk", phase="prepare", workers=1
        ):
            pass
        reporter.failure("ext4", "bulk", 0, RuntimeError("boom"))

        text = stream.getvalue()
        self.assertNotIn("bulk/prepare", text)
        self.assertIn("FAILED", text)

    def test_commands_are_reported_only_when_verbose(self) -> None:
        quiet, quiet_stream = self._reporter()
        verbose, verbose_stream = self._reporter(Level.VERBOSE)
        for reporter in (quiet, verbose):
            reporter.command("ext4-format", ("mkfs.ext4", "/dev/loop0"), 0, 1.5)

        self.assertEqual(quiet_stream.getvalue(), "")
        self.assertIn("ext4-format", verbose_stream.getvalue())

    def test_a_broken_stream_never_reaches_the_run(self) -> None:
        class Exploding(io.StringIO):
            def write(self, text: str) -> int:
                raise OSError("no terminal")

        reporter = RunReporter(stream=Exploding(), level=Level.NORMAL, color=False)
        reporter.section("matrix")
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        with reporter.phase(
            filesystem="ext4", scenario="bulk", phase="prepare", workers=1
        ):
            pass
        reporter.end_section()
        reporter.summary(
            run_id="test",
            failures=[],
            kernel_findings=0,
            publication_eligible=False,
            run_dir=Path("/tmp"),
        )

        self.assertTrue(reporter.degraded)
        self.assertEqual(reporter.completed_phases, 1)

    def test_a_failing_phase_is_counted_and_marked(self) -> None:
        reporter, stream = self._reporter()
        with self.assertRaises(RuntimeError):
            with reporter.phase(
                filesystem="ext4", scenario="bulk", phase="prepare", workers=1
            ):
                raise RuntimeError("boom")

        self.assertEqual(reporter.failed_phases, 1)
        self.assertIn("FAILED", stream.getvalue())

    def test_summary_reports_the_phases_that_were_reached(self) -> None:
        reporter, stream = self._reporter()
        reporter.announce_plan(
            build_plan(load_config(REPOSITORY / "bench/config/quick.toml"), ("ext4",))
        )
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        with reporter.phase(
            filesystem="ext4", scenario="bulk", phase="prepare", workers=1
        ):
            pass
        reporter.summary(
            run_id="test",
            failures=[],
            kernel_findings=2,
            publication_eligible=False,
            run_dir=Path("/tmp/run"),
        )

        text = stream.getvalue()
        self.assertIn("1/30 planned phases were reached", text)
        self.assertIn("kernel log: 2 relevant findings", text)
        self.assertIn("analyze /tmp/run", text)

    def test_each_unit_reports_its_own_elapsed_time(self) -> None:
        reporter, stream = self._reporter()
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        for phase in ("prepare", "cold-read"):
            with reporter.phase(
                filesystem="ext4", scenario="bulk", phase=phase, workers=1
            ):
                pass
        reporter.unit_start(
            matrix="common", filesystem="btrfs", scenario="bulk", repetition=0
        )
        with reporter.phase(
            filesystem="btrfs", scenario="bulk", phase="prepare", workers=1
        ):
            pass
        reporter.summary(
            run_id="test",
            failures=[],
            kernel_findings=0,
            publication_eligible=False,
            run_dir=Path("/tmp/run"),
        )

        text = stream.getvalue()
        self.assertIn("unit total (2 phases)", text)
        self.assertIn("unit total (1 phase)", text)
        # the first unit closes above the second headline, the last one at summary
        self.assertLess(text.index("unit total (2 phases)"), text.index("btrfs"))

    def test_sections_report_their_elapsed_time_once(self) -> None:
        reporter, stream = self._reporter()
        reporter.section("preflight")
        reporter.step("benchmark binaries ok")
        reporter.section("matrix")
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        reporter.end_section()
        reporter.summary(
            run_id="test",
            failures=[],
            kernel_findings=0,
            publication_eligible=False,
            run_dir=Path("/tmp/run"),
        )

        text = stream.getvalue()
        self.assertEqual(text.count("preflight total"), 1)
        self.assertEqual(text.count("matrix total"), 1)
        self.assertEqual(text.count("unit total"), 1)
        self.assertLess(text.index("preflight total"), text.index("matrix"))
        self.assertLess(text.index("unit total"), text.index("matrix total"))

    def test_quiet_drops_unit_and_section_totals(self) -> None:
        reporter, stream = self._reporter(Level.QUIET)
        reporter.section("matrix")
        reporter.unit_start(
            matrix="common", filesystem="ext4", scenario="bulk", repetition=0
        )
        reporter.end_section()
        reporter.summary(
            run_id="test",
            failures=[],
            kernel_findings=0,
            publication_eligible=False,
            run_dir=Path("/tmp/run"),
        )

        text = stream.getvalue()
        self.assertNotIn("unit total", text)
        self.assertNotIn("matrix total", text)
        self.assertIn("summary  test", text)

    def test_durations_read_as_time(self) -> None:
        self.assertEqual(format_duration(3.14), "3.1s")
        self.assertEqual(format_duration(305), "5m05s")
        self.assertEqual(format_duration(7200), "2h00m")


class PhaseMetricsTests(unittest.TestCase):
    def test_only_rows_written_by_this_phase_are_read(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.jsonl"
            path.write_text(
                json.dumps(result(workers=1, throughput=10.0)) + "\n",
                encoding="utf-8",
            )
            reader = PhaseMetricsReader()
            reader.start(path)
            with path.open("a", encoding="utf-8") as stream:
                stream.write(
                    json.dumps(result(workers=1, throughput=99.0)) + "\n"
                )

            record = reader.collect(path)

            self.assertIsNotNone(record)
            self.assertEqual(record["durable_throughput_mib_s"], 99.0)

    def test_the_aggregate_row_wins_over_worker_rows(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.jsonl"
            reader = PhaseMetricsReader()
            reader.start(path)
            worker = dict(result(workers=4, throughput=10.0), worker_id=0)
            aggregate = result(workers=4, throughput=40.0)
            path.write_text(
                json.dumps(worker) + "\n" + json.dumps(aggregate) + "\n"
                + json.dumps(dict(worker, worker_id=1)) + "\n",
                encoding="utf-8",
            )

            record = reader.collect(path)

            self.assertEqual(record["durable_throughput_mib_s"], 40.0)

    def test_a_truncated_line_is_left_for_the_next_read(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.jsonl"
            reader = PhaseMetricsReader()
            reader.start(path)
            path.write_text('{"filesystem": "bsfs", "scen', encoding="utf-8")

            self.assertIsNone(reader.collect(path))

            with path.open("a", encoding="utf-8") as stream:
                stream.write('ario": "bulk"}\n')

            self.assertEqual(
                reader.collect(path),
                {"filesystem": "bsfs", "scenario": "bulk"},
            )

    def test_a_missing_file_is_not_fatal(self) -> None:
        reader = PhaseMetricsReader()
        missing = Path("/nonexistent/results.jsonl")
        reader.start(missing)

        self.assertIsNone(reader.collect(missing))

    def test_metadata_phases_are_summarized_as_operations(self) -> None:
        record = {
            "durable_throughput_mib_s": 0.02,
            "durable_iops": 18200.0,
            "latency_p99_ns": 1_400_000,
            "validation_status": "passed",
        }

        self.assertEqual(describe_metrics(record), "18.2k ops/s  p99 1.40ms")

    def test_a_failed_validation_is_shouted_about(self) -> None:
        self.assertEqual(
            describe_metrics({"validation_status": "failed"}),
            "VALIDATION failed",
        )


class CommandDurationTests(unittest.TestCase):
    def test_commands_record_their_duration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = CommandRunner(Path(directory))
            runner.run(["true"], label="probe")

            record = json.loads(
                (Path(directory) / "commands.jsonl").read_text(encoding="utf-8")
            )

            self.assertGreaterEqual(record["duration_s"], 0.0)
            self.assertEqual(record["returncode"], 0)

    def test_a_failing_command_leaves_a_stderr_log_to_point_at(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = CommandRunner(Path(directory))
            runner.run(["false"], check=False, label="probe")

            self.assertIsNotNone(runner.last_error_log)
            self.assertTrue(runner.last_error_log.exists())


if __name__ == "__main__":
    unittest.main()
