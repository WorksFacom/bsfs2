from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
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
from bench.orchestration.command import CommandRunner
from bench.orchestration.config import load_config
from bench.orchestration.preflight import (
    BenchmarkBinaryError,
    BsfsModuleError,
    build_module_argv,
    validate_bsfs_module,
    validate_benchmark_binaries,
)
from bench.orchestration.safety import (
    SafetyError,
    cyclic_schedule,
    validate_loop_image,
)
from bench.orchestration.topology import physical_cores, sibling_conflicts
from bench.orchestration.run import (
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
    def test_loadable_module_is_built_loaded_and_unloaded(self) -> None:
        module = REPOSITORY / "bsfs2.ko"
        with (
            patch("bench.orchestration.preflight._run_capture") as capture,
            patch(
                "bench.orchestration.preflight._module_loaded",
                return_value=False,
            ),
        ):
            capture.side_effect = [
                subprocess.CompletedProcess(BUILD_ARGV, 0, "", ""),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 1, "", ""),
                subprocess.CompletedProcess(["insmod", str(module)], 0, "", ""),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 0, "", ""),
            ]

            validate_bsfs_module(REPOSITORY)

        self.assertEqual(
            capture.call_args_list,
            [
                call(BUILD_ARGV),
                call(["rmmod", "bsfs2"]),
                call(["insmod", str(module)]),
                call(["rmmod", "bsfs2"]),
            ],
        )

    def test_build_pins_pwd_to_the_repository(self) -> None:
        """make -C moves CURDIR but not PWD, and the kernel build uses
        M=$(PWD), so a launcher started elsewhere would build the wrong tree."""
        self.assertIn(f"PWD={REPOSITORY}", BUILD_ARGV)
        self.assertEqual(BUILD_ARGV[:3], ["make", "-C", str(REPOSITORY)])

    def test_load_failure_reports_kernel_and_vermagic(self) -> None:
        module = REPOSITORY / "bsfs2.ko"
        with (
            patch("bench.orchestration.preflight._run_capture") as capture,
            patch(
                "bench.orchestration.preflight._module_loaded",
                return_value=False,
            ),
        ):
            capture.side_effect = [
                subprocess.CompletedProcess(BUILD_ARGV, 0, "", ""),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 1, "", ""),
                subprocess.CompletedProcess(
                    ["insmod", str(module)], 1, "", "Invalid module format"
                ),
                subprocess.CompletedProcess(
                    ["modinfo", "-F", "vermagic", str(module)],
                    0,
                    "6.18.15-dirty SMP preempt mod_unload\n",
                    "",
                ),
            ]

            with self.assertRaises(BsfsModuleError) as raised:
                validate_bsfs_module(REPOSITORY)

        message = str(raised.exception)
        self.assertIn("Invalid module format", message)
        self.assertIn("module vermagic: 6.18.15-dirty", message)
        self.assertIn("KDIR", message)

    def test_busy_module_is_not_blamed_on_kdir(self) -> None:
        module = REPOSITORY / "bsfs2.ko"
        with (
            patch("bench.orchestration.preflight._run_capture") as capture,
            patch(
                "bench.orchestration.preflight._module_loaded",
                return_value=True,
            ),
        ):
            capture.side_effect = [
                subprocess.CompletedProcess(BUILD_ARGV, 0, "", ""),
                subprocess.CompletedProcess(
                    ["rmmod", "bsfs2"], 1, "", "rmmod: ERROR: Module bsfs2 is in use"
                ),
                subprocess.CompletedProcess(
                    ["modinfo", "-F", "vermagic", str(module)], 0, "6.18.15\n", ""
                ),
            ]

            with self.assertRaises(BsfsModuleError) as raised:
                validate_bsfs_module(REPOSITORY)

        message = str(raised.exception)
        self.assertIn("still in use", message)
        self.assertNotIn("KDIR", message)
        # insmod must not run against a module that is already loaded and busy
        self.assertNotIn(
            call(["insmod", str(module)]), capture.call_args_list
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
            )
            with (
                patch("bench.orchestration.run.parse_args", return_value=args),
                patch("bench.orchestration.run.os.geteuid", return_value=0),
                patch("bench.orchestration.run.validate_benchmark_binaries"),
                patch(
                    "bench.orchestration.run.validate_bsfs_module",
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
            )
            with (
                patch("bench.orchestration.run.parse_args", return_value=args),
                patch("bench.orchestration.run.os.geteuid", return_value=0),
                patch("bench.orchestration.run.validate_benchmark_binaries"),
                patch("bench.orchestration.run.validate_bsfs_module") as module,
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

        payload = json.loads(
            report.split('id="report-data" type="application/json">')[1]
            .split("</script>")[0]
            .replace("\\u003c", "<")
            .replace("\\u003e", ">")
            .replace("\\u0026", "&")
        )

        # a scenario-wide failure has no phase, so it belongs to every phase
        self.assertEqual(
            payload["failedByPhase"],
            {"bulk": {"": ["zfs"], "verify": ["btrfs"]}},
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


if __name__ == "__main__":
    unittest.main()
