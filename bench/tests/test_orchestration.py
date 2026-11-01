from __future__ import annotations

import argparse
import dataclasses
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
    derive_unstable,
    derive_summary,
    load_records,
)
from bench.orchestration.report import render_report
from bench.orchestration.command import (
    CommandError,
    CommandResult,
    CommandRunner,
)
from bench.orchestration.config import load_config
from bench.orchestration.preflight import (
    BenchmarkBinaryError,
    validate_benchmark_binaries,
)
from bench.orchestration.modules import (
    BsfsModuleError,
    CustomBsfsArtifacts,
    alloc_strategies,
    build_module_argv,
    build_modules,
    custom_artifact_paths,
    define_custom_artifacts,
    module_path,
    stage_custom_artifacts,
)
from bench.orchestration.manifest import _artifacts, _custom_artifacts
from bench.orchestration.conditioning import DeviceConditioner
from bench.orchestration.safety import (
    DeviceBusy,
    SafetyError,
    cyclic_schedule,
    device_mountpoints,
    validate_loop_image,
)
from bench.orchestration.udev import UdisksIgnore
from bench.orchestration.topology import physical_cores, sibling_conflicts
from bench.orchestration.filesystems import (
    ADAPTERS,
    BSFS_FILESYSTEMS,
    FilesystemAdapter,
    FilesystemContext,
    bsfs_targets_by_strategy,
    create_adapter,
)
from bench.orchestration.plan import COLD, REOPEN, SCENARIOS, build_plan
from bench.orchestration.progress import (
    Level,
    PhaseMetricsReader,
    RunReporter,
    describe_metrics,
    format_duration,
)
from bench.orchestration.run import (
    BenchmarkRun,
    custom_bsfs_definitions,
    main,
    sudo_invoker_owner,
    transfer_result_ownership,
)

BUILD_ARGV = build_module_argv(REPOSITORY)


class ConfigTests(unittest.TestCase):
    def test_default_config_is_valid(self) -> None:
        config = load_config(REPOSITORY / "bench/config/default.toml")

        self.assertEqual(config.repetitions, 10)
        self.assertEqual(len(config.seeds), 10)
        self.assertEqual(config.filesystems, ("bsfs", "ext4", "btrfs", "zfs"))
        self.assertFalse(config.recovery)
        self.assertGreater(config.phase_timeout_s, 0)
        self.assertGreaterEqual(len(config.concurrency_cpus), 4)

    def test_worker_cpus_land_on_distinct_physical_cores(self) -> None:
        config = load_config(REPOSITORY / "bench/config/default.toml")

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

    def test_a_command_past_its_timeout_is_killed_and_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = CommandRunner(Path(directory))
            with self.assertRaisesRegex(CommandError, "timed out") as raised:
                runner.run(["sleep", "30"], label="hang", timeout=0.2)

            self.assertTrue(raised.exception.result.timed_out)
            self.assertIn(
                "timed out after 0.2 s",
                (Path(directory) / "logs/00001-hang.stderr").read_text(),
            )

    def test_the_default_timeout_applies_when_none_is_passed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runner = CommandRunner(Path(directory), default_timeout=0.2)
            with self.assertRaises(CommandError):
                runner.run(["sleep", "30"], check=False, label="hang")


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
        """a failed umount leaves a live filesystem under run_dir, and chowning through it hits the device."""
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

    def test_an_executable_runner_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            self.create_binary(repository, "bench-posix")

            validate_benchmark_binaries(repository)

    def test_missing_runner_is_rejected_with_build_command(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(BenchmarkBinaryError, "make -C bench"):
                validate_benchmark_binaries(Path(directory))

    def test_non_executable_runner_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            binary = self.create_binary(repository, "bench-posix")
            binary.chmod(0o644)

            with self.assertRaisesRegex(BenchmarkBinaryError, "make -C bench"):
                validate_benchmark_binaries(repository)


class BsfsModulePreflightTests(unittest.TestCase):
    def build_argv(self, strategy: str) -> list[str]:
        return build_module_argv(REPOSITORY, strategy)

    def patched(self, *, loaded: bool = False, built_as_ok: bool = True):
        """Patches out writes to bench/build and the strategy check."""
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
                # gates and QEMU scripts boot the default build
                call(BUILD_ARGV),
            ],
        )
        copied.assert_called_once_with(REPOSITORY / "bsfs2.ko", stash)

    def test_strategy_build_pins_the_allocator_and_clears_the_counters(
        self,
    ) -> None:
        argv = self.build_argv("twolevel")

        self.assertIn("BSFS_ALLOC=twolevel", argv)
        # counters sit on the measured fast path, and local.mk must not turn them back on
        self.assertIn("BSFS_ALLOC_STATS=", argv)

    def test_build_runs_make_in_the_repository(self) -> None:
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


def custom_artifacts_stub(name: str) -> CustomBsfsArtifacts:
    return CustomBsfsArtifacts(
        name=name,
        source_module=Path(f"/source/{name}/bsfs2.ko"),
        source_formatter=Path(f"/source/{name}/mkfs.bsfs"),
        module=REPOSITORY / f"bench/build/custom/{name}/bsfs2.ko",
        formatter=REPOSITORY / f"bench/build/custom/{name}/mkfs.bsfs",
        module_sha256="a" * 64,
        formatter_sha256="b" * 64,
        module_name="bsfs2",
        vermagic="6.18.15 SMP mod_unload",
    )


class CustomBsfsArtifactTests(unittest.TestCase):
    def sources(self, directory: Path) -> tuple[Path, Path]:
        module = directory / "source" / "bsfs2.ko"
        formatter = directory / "source" / "mkfs.bsfs"
        module.parent.mkdir(parents=True)
        module.write_bytes(b"custom module")
        formatter.write_bytes(b"custom formatter")
        formatter.chmod(0o755)
        return module, formatter

    def definition(
        self, repository: Path, module: Path, formatter: Path
    ) -> CustomBsfsArtifacts:
        with (
            patch(
                "bench.orchestration.modules._module_name",
                return_value="bsfs2",
            ),
            patch(
                "bench.orchestration.modules._module_vermagic",
                return_value="6.18.15 SMP mod_unload",
            ),
        ):
            return define_custom_artifacts(
                repository, "bsfs-maple", module, formatter
            )

    def test_definition_requires_a_matching_named_executable_pair(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository = root / "repository"
            module, formatter = self.sources(root)

            artifacts = self.definition(repository, module, formatter)

            self.assertEqual(artifacts.source_module, module.resolve())
            self.assertEqual(artifacts.source_formatter, formatter.resolve())
            self.assertEqual(
                (artifacts.module, artifacts.formatter),
                custom_artifact_paths(repository, "bsfs-maple"),
            )
            self.assertEqual(artifacts.module_name, "bsfs2")
            self.assertEqual(len(artifacts.module_sha256), 64)
            self.assertEqual(len(artifacts.formatter_sha256), 64)

    def test_definition_rejects_bad_names_and_non_executable_formatters(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module, formatter = self.sources(root)
            with self.assertRaisesRegex(BsfsModuleError, "invalid custom"):
                define_custom_artifacts(
                    root / "repository", "maple", module, formatter
                )

            formatter.chmod(0o644)
            with patch(
                "bench.orchestration.modules._module_name",
                return_value="bsfs2",
            ):
                with self.assertRaisesRegex(BsfsModuleError, "not executable"):
                    define_custom_artifacts(
                        root / "repository",
                        "bsfs-maple",
                        module,
                        formatter,
                    )

    def test_cli_definitions_reject_collisions_and_duplicate_names(self) -> None:
        with self.assertRaisesRegex(BsfsModuleError, "built-in"):
            custom_bsfs_definitions(
                REPOSITORY,
                [["bsfs-heap", "/module", "/formatter"]],
            )

        sentinel = object()
        with patch(
            "bench.orchestration.run.define_custom_artifacts",
            return_value=sentinel,
        ):
            with self.assertRaisesRegex(BsfsModuleError, "more than once"):
                custom_bsfs_definitions(
                    REPOSITORY,
                    [
                        ["bsfs-maple", "/one.ko", "/one.mkfs"],
                        ["bsfs-maple", "/two.ko", "/two.mkfs"],
                    ],
                )

    def test_staging_copies_hashes_and_probes_the_frozen_module(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository = root / "repository"
            module, formatter = self.sources(root)
            artifacts = self.definition(repository, module, formatter)

            with (
                patch(
                    "bench.orchestration.modules._module_name",
                    return_value="bsfs2",
                ),
                patch(
                    "bench.orchestration.modules._module_vermagic",
                    return_value="6.18.15 SMP mod_unload",
                ),
                patch("bench.orchestration.modules._probe_load") as probe,
            ):
                staged = stage_custom_artifacts(artifacts)

            self.assertEqual(staged.module.read_bytes(), module.read_bytes())
            self.assertEqual(
                staged.formatter.read_bytes(), formatter.read_bytes()
            )
            self.assertTrue(staged.formatter.stat().st_mode & 0o111)
            probe.assert_called_once_with(
                staged.module,
                expected_filesystem="bsfs",
                remedy=(
                    "Rebuild the custom bsfs-maple module against the running "
                    "kernel, then pass the rebuilt artifact and retry."
                ),
            )

    def test_staging_rejects_a_source_changed_since_it_was_supplied(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository = root / "repository"
            module, formatter = self.sources(root)
            artifacts = self.definition(repository, module, formatter)

            module.write_bytes(b"rebuilt between definition and staging")

            with (
                patch(
                    "bench.orchestration.modules._module_name",
                    return_value="bsfs2",
                ),
                patch("bench.orchestration.modules._probe_load") as probe,
            ):
                with self.assertRaisesRegex(
                    BsfsModuleError, "changed since it was supplied"
                ):
                    stage_custom_artifacts(artifacts)

            probe.assert_not_called()

    def test_staging_reports_an_unusable_destination_as_a_module_error(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository = root / "repository"
            module, formatter = self.sources(root)
            artifacts = self.definition(repository, module, formatter)
            artifacts.module.parent.parent.mkdir(parents=True)
            artifacts.module.parent.write_bytes(b"not a directory")

            with self.assertRaisesRegex(BsfsModuleError, "cannot stage artifacts"):
                stage_custom_artifacts(artifacts)

    def test_a_failed_modinfo_is_not_reported_as_a_wrong_module_name(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module, formatter = self.sources(root)
            with patch(
                "bench.orchestration.modules._run_capture",
                return_value=subprocess.CompletedProcess(
                    ["modinfo"], 1, "", "ERROR: could not get modinfo"
                ),
            ):
                with self.assertRaisesRegex(
                    BsfsModuleError,
                    "custom BSFS bsfs-maple: modinfo could not read",
                ):
                    define_custom_artifacts(
                        root / "repository", "bsfs-maple", module, formatter
                    )

    def test_a_custom_definition_cannot_shadow_a_built_in_target(self) -> None:
        context = FilesystemContext(
            runner=CommandRunner(
                REPOSITORY / "bench/results/unused", dry_run=True
            ),
            repository=REPOSITORY,
            device="/dev/loop-dry-run",
            stable_device="/dev/loop-dry-run",
            mountpoint=Path("/mnt/bsfs-bench"),
            run_id="test",
        )
        with self.assertRaisesRegex(ValueError, "shadows a built-in"):
            create_adapter("ext4", context, {"ext4": custom_artifacts_stub("ext4")})

    def test_probe_unloads_a_module_that_does_not_register_bsfs(self) -> None:
        from bench.orchestration.modules import _probe_load

        module = Path("/tmp/custom-bsfs2.ko")
        with (
            patch("bench.orchestration.modules._run_capture") as run,
            patch(
                "bench.orchestration.modules._module_loaded", return_value=False
            ),
            patch(
                "bench.orchestration.modules._filesystem_registered",
                return_value=False,
            ),
        ):
            run.side_effect = [
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 1, "", ""),
                subprocess.CompletedProcess(
                    ["insmod", str(module)], 0, "", ""
                ),
                subprocess.CompletedProcess(["rmmod", "bsfs2"], 0, "", ""),
            ]

            with self.assertRaisesRegex(BsfsModuleError, "did not register"):
                _probe_load(module, expected_filesystem="bsfs")

        self.assertEqual(run.call_args_list[-1], call(["rmmod", "bsfs2"]))

    def test_custom_adapter_uses_the_staged_pair_and_hash_version(self) -> None:
        recorded: list[tuple[str, ...]] = []

        class Recorder:
            def dry_command(self, argv: tuple[str, ...]) -> None:
                recorded.append(argv)

        artifacts = CustomBsfsArtifacts(
            name="bsfs-maple",
            source_module=Path("/source/bsfs2.ko"),
            source_formatter=Path("/source/mkfs.bsfs"),
            module=Path("/staged/bsfs2.ko"),
            formatter=Path("/staged/mkfs.bsfs"),
            module_sha256="a" * 64,
            formatter_sha256="b" * 64,
            module_name="bsfs2",
            vermagic="6.18.15 SMP mod_unload",
        )
        runner = CommandRunner(
            REPOSITORY / "bench/results/unused",
            dry_run=True,
            reporter=Recorder(),
        )
        adapter = create_adapter(
            "bsfs-maple",
            FilesystemContext(
                runner=runner,
                repository=REPOSITORY,
                device="/dev/loop-dry-run",
                stable_device="/dev/loop-dry-run",
                mountpoint=Path("/mnt/bsfs-bench"),
                run_id="test",
            ),
            {"bsfs-maple": artifacts},
        )

        adapter.format()
        adapter.mount()

        self.assertIn(
            ("/staged/mkfs.bsfs", "/dev/loop-dry-run"), recorded
        )
        self.assertIn(("insmod", "/staged/bsfs2.ko"), recorded)
        self.assertEqual(
            adapter.version(), f"custom module={'a' * 16} mkfs={'b' * 16}"
        )

    def test_manifest_records_custom_sources_and_only_requested_extras(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository = root / "repository"
            module, formatter = self.sources(root)
            artifacts = self.definition(repository, module, formatter)
            artifacts.module.parent.mkdir(parents=True)
            artifacts.module.write_bytes(module.read_bytes())
            artifacts.formatter.write_bytes(formatter.read_bytes())

            custom = _custom_artifacts({"bsfs-maple": artifacts})
            built = _artifacts(
                repository, (artifacts.module, artifacts.formatter)
            )

            self.assertEqual(
                custom["bsfs-maple"]["source_module"]["sha256"],
                artifacts.module_sha256,
            )
            self.assertTrue(
                built["bench/build/custom/bsfs-maple/bsfs2.ko"]["present"]
            )
            self.assertTrue(
                built["bench/build/custom/bsfs-maple/mkfs.bsfs"]["present"]
            )


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
        # "bsfs" and "bsfs-linear" are the same module
        self.assertEqual(
            bsfs_targets_by_strategy(
                ("bsfs", "bsfs-linear", "bsfs-heap", "ext4")
            ),
            {"linear": ("bsfs", "bsfs-linear"), "heap": ("bsfs-heap",)},
        )


class AnalyzeEntryPointTests(unittest.TestCase):
    def test_analyze_runs_as_a_script_from_an_unrelated_directory(self) -> None:
        """analyze.py has a shebang, so it must work without the repo root on sys.path."""
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
    def custom_artifacts(self) -> CustomBsfsArtifacts:
        return custom_artifacts_stub("bsfs-maple")

    def test_module_preflight_failure_creates_no_run_directory(self) -> None:
        config = REPOSITORY / "bench/config/default.toml"
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

    def test_selected_custom_name_requires_a_cli_definition(self) -> None:
        args = argparse.Namespace(
            config=REPOSITORY / "bench/config/default.toml",
            device=None,
            loop_image=Path("/tmp/unused-custom-bench.img"),
            confirm=None,
            allow_size_outside_range=False,
            allow_shared_disk=False,
            run_dir=None,
            filesystems=("bsfs-maple",),
            custom_bsfs=None,
            dry_run=True,
            verbose=False,
            quiet=True,
            no_color=True,
        )
        with patch("bench.orchestration.run.parse_args", return_value=args):
            with self.assertRaisesRegex(SystemExit, "--custom-bsfs"):
                main()

    def test_non_bsfs_selection_skips_module_preflight(self) -> None:
        config = REPOSITORY / "bench/config/default.toml"
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

    def test_custom_preflight_failure_creates_no_run_directory(self) -> None:
        config = REPOSITORY / "bench/config/default.toml"
        artifacts = self.custom_artifacts()
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
                filesystems=("bsfs-maple",),
                custom_bsfs=[
                    ["bsfs-maple", "/source/bsfs2.ko", "/source/mkfs.bsfs"]
                ],
                dry_run=False,
                verbose=False,
                quiet=True,
                no_color=True,
            )
            with (
                patch("bench.orchestration.run.parse_args", return_value=args),
                patch(
                    "bench.orchestration.run.custom_bsfs_definitions",
                    return_value={"bsfs-maple": artifacts},
                ),
                patch("bench.orchestration.run.os.geteuid", return_value=0),
                patch("bench.orchestration.run.validate_benchmark_binaries"),
                patch(
                    "bench.orchestration.run.stage_custom_artifacts",
                    side_effect=BsfsModuleError("invalid custom module format"),
                ),
                patch("bench.orchestration.run.CommandRunner") as runner,
            ):
                self.assertEqual(main(), 2)

            self.assertFalse(run_dir.exists())
            self.assertFalse(loop_image.exists())
            runner.assert_not_called()

    def test_unselected_custom_definition_is_not_staged(self) -> None:
        config = REPOSITORY / "bench/config/default.toml"
        artifacts = self.custom_artifacts()
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
                custom_bsfs=[
                    ["bsfs-maple", "/source/bsfs2.ko", "/source/mkfs.bsfs"]
                ],
                dry_run=False,
                verbose=False,
                quiet=True,
                no_color=True,
            )
            with (
                patch("bench.orchestration.run.parse_args", return_value=args),
                patch(
                    "bench.orchestration.run.custom_bsfs_definitions",
                    return_value={"bsfs-maple": artifacts},
                ),
                patch("bench.orchestration.run.os.geteuid", return_value=0),
                patch("bench.orchestration.run.validate_benchmark_binaries"),
                patch(
                    "bench.orchestration.run.stage_custom_artifacts"
                ) as stage,
                patch("bench.orchestration.run.CommandRunner"),
                patch(
                    "bench.orchestration.run.inspect_physical_device",
                    side_effect=SafetyError("stop after preflight"),
                ),
            ):
                self.assertEqual(main(), 2)

            stage.assert_not_called()


def result(
    *,
    workers: int,
    throughput: float,
    worker_id: int | None = None,
    sectors_written: int | None = None,
    durable_window: bool = True,
    work_scale: int = 1,
) -> dict[str, object]:
    return {
        "work_scale": work_scale,
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
                "hostname": "test-host",
                "config": {"repetitions": 1},
                "kernel": {"release": "test-kernel"},
                "filesystem_schedule": [
                    {"repetition": 0, "order": ["bsfs", "ext4"]}
                ],
            },
        )

        self.assertIn("BSFS benchmark report: run-1", report)
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

    def test_report_charts_fit_without_scrolling_and_expose_order_controls(
        self,
    ) -> None:
        """a filesystem scrolled off the side reads as "not run", so charts grow down, not sideways."""
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[],
            skipped=[],
            manifest={},
        )

        self.assertNotIn("overflow-x: auto", report)
        self.assertNotIn("minWidth", report)
        self.assertIn('id="sort-value"', report)
        self.assertIn('id="sort-name"', report)
        self.assertIn('id="expand-all"', report)
        self.assertIn('id="collapse-all"', report)
        self.assertIn('let chartSort = "value";', report)
        self.assertIn('#scenario-groups details', report)

    def test_every_metric_records_which_end_is_better(self) -> None:
        """value order flips per metric, read from the flag rather than the direction text."""
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            failures=[],
            skipped=[],
            manifest={},
        )

        for metric in embedded_report_data(report)["metrics"]:
            self.assertEqual(metric[5], metric[3] == "Lower is better", metric[0])

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
        """a bare NaN makes JSON.parse throw and blanks the whole page, not just one number."""
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
        self.assertEqual(
            descriptions["future-workload"]["novel-phase"],
            'Compares filesystems for benchmark phase "novel-phase".',
        )

    def test_unstable_phases_reach_the_report(self) -> None:
        unstable = [{"filesystem": "ext4", "phase": "edit", "reasons": "wide-range"}]
        report = render_report(
            run_dir=Path("/results/run-1"),
            record_count=0,
            summary=[],
            scaling=[],
            unstable=unstable,
            failures=[],
            skipped=[],
            manifest={},
        )

        self.assertEqual(embedded_report_data(report)["unstable"], unstable)
        self.assertIn('id="unstable"', report)

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
        self.assertIn("page-cache ingest", metrics["throughput_mib_s"])
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

    def test_scaling_separates_size_and_worker_effects(self) -> None:
        records = [
            result(workers=1, throughput=80.0),
            result(workers=1, throughput=20.0, work_scale=4),
            result(workers=4, throughput=30.0),
        ]

        row = derive_scaling(derive_summary(records))[0]

        self.assertEqual(row["one_worker_same_total_mib_s"], 20.0)
        self.assertEqual(row["scaling_ratio"], 0.375)
        self.assertEqual(row["size_effect_ratio"], 0.25)
        self.assertEqual(row["worker_effect_ratio"], 1.5)

    def test_scaling_tolerates_runs_without_the_same_total_shape(self) -> None:
        records = [
            {k: v for k, v in result(workers=w, throughput=t).items() if k != "work_scale"}
            for w, t in ((1, 10.0), (4, 30.0))
        ]

        row = derive_scaling(derive_summary(records))[0]

        self.assertEqual(row["scaling_ratio"], 3.0)
        self.assertIsNone(row["size_effect_ratio"])
        self.assertIsNone(row["worker_effect_ratio"])

    def test_scaling_falls_back_to_the_operation_rate_for_reads(self) -> None:
        records = [
            result(workers=1, throughput=10.0, durable_window=False),
            result(workers=4, throughput=30.0, durable_window=False),
        ]

        scaling = derive_scaling(derive_summary(records))

        self.assertEqual(scaling[0]["rate_basis"], "operation")
        self.assertEqual(scaling[0]["one_worker_mib_s"], 40.0)
        self.assertEqual(scaling[0]["four_worker_mib_s"], 120.0)

    def test_durable_rates_are_dropped_without_a_flush(self) -> None:
        # schema 1 records carry a durable rate even when the phase never flushed
        row = derive_summary(
            [result(workers=1, throughput=10.0, durable_window=False)]
        )[0]

        self.assertIsNone(row["durable_throughput_mib_s_median"])
        self.assertEqual(row["throughput_mib_s_median"], 40.0)

    def test_short_and_spread_out_phases_are_flagged(self) -> None:
        def timed(workers: int, throughput: float, total_ns: float) -> dict[str, object]:
            return {
                **result(workers=workers, throughput=throughput),
                "durable_total_ns": total_ns,
            }

        steady = [timed(1, 100.0, 1e9), timed(1, 110.0, 1e9), timed(1, 105.0, 1e9)]
        short = [timed(4, 100.0, 5e6), timed(4, 101.0, 5e6)]
        worker = {**timed(2, 1.0, 1e9), "worker_id": 0}
        wide = {
            **timed(2, 97.0, 1e9),
            "phase": "edit",
        }
        wide_high = {**timed(2, 230.0, 1e9), "phase": "edit"}

        rows = derive_unstable(
            derive_summary([*steady, *short, worker, wide, wide_high])
        )

        self.assertEqual(
            {(row["worker_count"], row["phase"]): row["reasons"] for row in rows},
            {(4, "write"): "short", (2, "edit"): "wide-range"},
        )

    def test_unflushed_metadata_phases_use_their_operation_rate(self) -> None:
        record = {
            **result(workers=1, throughput=0.0, durable_window=False),
            "throughput_mib_s": 0.0,
            "iops": 500.0,
            "operation_path_ns": 2e6,
        }

        row = derive_unstable(derive_summary([record]))[0]

        self.assertEqual(row["rate_metric"], "iops")
        self.assertEqual(row["reasons"], "short")

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
    def _config(self, name: str = "default", **changes: Any) -> Any:
        config = load_config(REPOSITORY / f"bench/config/{name}.toml")
        return dataclasses.replace(config, **changes)

    def _walk(
        self,
        config: Any,
        selected: tuple[str, ...],
        *,
        custom_bsfs: tuple[str, ...] = (),
        fail: tuple[str, str] | None = None,
        busy: Exception | None = None,
    ) -> tuple[list[tuple[str, str, str, int]], BenchmarkRun]:
        """Runs the plan offline and returns every step in execution order."""
        executed: list[tuple[str, str, str, int]] = []
        current: dict[str, Any] = {}

        def phase(
            self, adapter, scenario, phase, repetition, seed, workers=1, work_scale=1
        ):
            current.update(scenario=scenario, workers=workers)
            if fail == (adapter.name, scenario):
                raise RuntimeError("phase failed")
            executed.append((adapter.name, scenario, phase, workers))

        def transition(marker: str):
            def record(adapter):
                executed.append(
                    (adapter.name, current["scenario"], marker, current["workers"])
                )

            return record

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
                reporter=reporter,
                custom_bsfs={
                    name: custom_artifacts_stub(name) for name in custom_bsfs
                },
            )
            with (
                patch(
                    "bench.orchestration.run.create_adapter",
                    side_effect=lambda name, context, custom=None: StubAdapter(
                        context, name
                    ),
                ),
                patch.object(
                    BenchmarkRun, "run_phase", autospec=True, side_effect=phase
                ),
                patch(
                    "bench.orchestration.run.cold_reset",
                    side_effect=transition(COLD),
                ),
                patch(
                    "bench.orchestration.run.reopen",
                    side_effect=transition(REOPEN),
                ),
                patch.object(
                    BenchmarkRun, "assert_device_free", side_effect=busy
                ),
            ):
                benchmark.run_plan(
                    build_plan(config, selected, custom_bsfs=custom_bsfs)
                )
        return executed, benchmark

    def test_default_plan_size(self) -> None:
        config = self._config()
        plan = build_plan(config, config.filesystems)

        # 4 filesystems x 10 repetitions x (3 common + 2 concurrency x 3 shapes)
        self.assertEqual(plan.total_units, 360)
        self.assertEqual(
            {unit.matrix for unit in plan.units}, {"common", "concurrency"}
        )

    def test_concurrency_shapes_include_one_worker_at_the_four_worker_total(self) -> None:
        config = self._config()
        plan = build_plan(config, ("ext4",))

        self.assertEqual(
            {
                (unit.workers, unit.work_scale)
                for unit in plan.units
                if unit.matrix == "concurrency"
            },
            {(1, 1), (1, 4), (4, 1)},
        )

    def test_recovery_adds_one_trailing_unit_per_bsfs_target(self) -> None:
        config = self._config(recovery=True)
        plan = build_plan(config, config.filesystems)

        self.assertEqual(plan.total_units, 361)
        last = plan.units[-1]
        self.assertEqual(
            (last.matrix, last.filesystem, last.scenario, last.repetition),
            ("recovery", "bsfs", "recovery-pressure", 0),
        )
        self.assertEqual(last.phases, ("run", "verify"))

    def test_every_scenario_ends_verified_and_starts_with_a_phase(self) -> None:
        for scenario, steps in SCENARIOS.items():
            self.assertFalse(steps[0].startswith("@"), scenario)
            self.assertFalse(steps[-1].startswith("@"), scenario)

    def test_the_run_executes_the_planned_steps_in_order(self) -> None:
        config = self._config(recovery=True)
        executed, benchmark = self._walk(config, config.filesystems)
        plan = build_plan(config, config.filesystems)

        self.assertEqual(
            executed,
            [
                (unit.filesystem, unit.scenario, step, unit.workers)
                for unit in plan.units
                for step in unit.steps
            ],
        )
        self.assertEqual(benchmark.failures, [])

    def test_cold_phases_follow_a_cache_drop(self) -> None:
        config = self._config()
        executed, _ = self._walk(config, ("ext4",))
        steps = [row[2] for row in executed if row[1] == "bulk"][:3]

        self.assertEqual(steps, ["prepare", COLD, "cold-read"])

    def test_a_failed_unit_is_recorded_and_the_run_continues(self) -> None:
        config = self._config(repetitions=1)
        executed, benchmark = self._walk(
            config, ("ext4", "btrfs"), fail=("ext4", "bulk")
        )

        self.assertEqual(
            [(f["filesystem"], f["scenario"]) for f in benchmark.failures],
            [("ext4", "bulk")],
        )
        self.assertNotIn("bulk", {row[1] for row in executed if row[0] == "ext4"})
        self.assertIn(("btrfs", "bulk", "verify", 1), executed)
        self.assertIn(("ext4", "metadata", "verify", 1), executed)

    def test_a_device_grabbed_from_outside_aborts_the_run(self) -> None:
        config = self._config(repetitions=1)
        executed, benchmark = self._walk(
            config, ("ext4", "btrfs"), busy=DeviceBusy("mounted elsewhere")
        )

        self.assertEqual(executed, [])
        self.assertEqual(len(benchmark.failures), 1)
        self.assertIn("mounted elsewhere", benchmark.failures[0]["error"])

    def test_every_allocator_variant_gets_a_recovery_unit(self) -> None:
        config = self._config("alloc", recovery=True)
        plan = build_plan(config, config.filesystems)

        self.assertEqual(
            [unit.filesystem for unit in plan.units if unit.matrix == "recovery"],
            [name for name in BSFS_FILESYSTEMS if name in config.filesystems],
        )

    def test_other_filesystems_never_get_recovery(self) -> None:
        plan = build_plan(self._config(recovery=True), ("ext4",))

        self.assertEqual(plan.filesystems, ("ext4",))
        self.assertEqual(
            {unit.matrix for unit in plan.units}, {"common", "concurrency"}
        )

    def test_custom_bsfs_gets_common_recovery_and_concurrency_units(self) -> None:
        plan = build_plan(
            self._config(recovery=True),
            ("bsfs-maple", "ext4"),
            custom_bsfs=("bsfs-maple",),
        )

        custom_units = [
            unit for unit in plan.units if unit.filesystem == "bsfs-maple"
        ]
        self.assertEqual(
            {unit.matrix for unit in custom_units},
            {"common", "recovery", "concurrency"},
        )
        self.assertEqual(
            {unit.workers for unit in custom_units if unit.matrix == "concurrency"},
            {1, 4},
        )

    def test_an_unknown_scenario_is_rejected_before_anything_runs(self) -> None:
        config = self._config(common_scenarios=("bulk", "nope"))
        with self.assertRaisesRegex(ValueError, "unknown scenario: nope"):
            build_plan(config, ("ext4",))


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
                config=load_config(REPOSITORY / "bench/config/default.toml"),
                runner=runner,
                device="/dev/loop-test",
                stable_device="/dev/loop-test",
                device_info=None,
                run_id="test",
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
                config=load_config(REPOSITORY / "bench/config/default.toml"),
                runner=runner,
                device="/dev/loop-test",
                stable_device="/dev/loop-test",
                device_info=None,
                run_id="test",
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
            build_plan(load_config(REPOSITORY / "bench/config/default.toml"), ("ext4",))
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
            build_plan(load_config(REPOSITORY / "bench/config/default.toml"), ("ext4",))
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
            build_plan(load_config(REPOSITORY / "bench/config/default.toml"), ("ext4",))
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
            run_dir=Path("/tmp/run"),
        )

        text = stream.getvalue()
        self.assertIn("1/360 planned phases were reached", text)
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


class RecordingRunner:
    dry_run = True

    def __init__(self) -> None:
        self.argvs: list[list[str]] = []

    def run(self, argv, **kwargs):
        self.argvs.append(list(argv))
        return CommandResult(tuple(argv), 0, "", "")


class UdevTests(unittest.TestCase):
    def test_conditioning_waits_for_udev_after_wiping(self) -> None:
        runner = RecordingRunner()
        DeviceConditioner(runner, "/dev/loop7", None, False).condition()

        self.assertEqual(runner.argvs[-2][0], "wipefs")
        self.assertEqual(runner.argvs[-1][:2], ["udevadm", "settle"])

    def test_only_mounts_of_the_device_itself_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            mountinfo = Path(directory) / "mountinfo"
            mountinfo.write_text(
                "36 25 7:0 / /run/media/user/disk rw - ext4 /dev/loop0 rw\n"
                "37 25 7:1 / /mnt/other rw - ext4 /dev/loop1 rw\n"
                "38 25 7:0 / /bench/mnt rw - ext4 /dev/loop0 rw\n",
                encoding="utf-8",
            )
            with patch(
                "bench.orchestration.safety._devnum", return_value=(7, 0)
            ):
                points = device_mountpoints("/dev/loop0", mountinfo)

        self.assertEqual(points, ["/run/media/user/disk", "/bench/mnt"])

    def test_udisks_rule_is_installed_applied_and_removed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            rules = Path(directory) / "rules.d"
            runner = CommandRunner(Path(directory) / "run")
            with (
                patch("bench.orchestration.udev.RULES_DIR", rules),
                patch.object(runner, "run") as run,
            ):
                ignore = UdisksIgnore(runner, "/dev/loop7", "20261014T041701Z")
                ignore.install()
                rule = rules / "99-bsfs-bench-20261014T041701Z.rules"

                self.assertEqual(
                    rule.read_text(encoding="ascii"),
                    'KERNEL=="loop7", ENV{UDISKS_IGNORE}="1"\n',
                )
                self.assertEqual(
                    [c.args[0] for c in run.call_args_list],
                    [
                        ["udevadm", "control", "--reload"],
                        [
                            "udevadm",
                            "trigger",
                            "--action=change",
                            "--settle",
                            "/dev/loop7",
                        ],
                    ],
                )

                ignore.remove()

                self.assertFalse(rule.exists())
                self.assertEqual(
                    run.call_args_list[-1].args[0],
                    ["udevadm", "control", "--reload"],
                )


if __name__ == "__main__":
    unittest.main()
