# BSFS benchmark suite

This directory compares mounted kernel BSFS, Ext4, Btrfs, and ZFS through one
POSIX runner and compares the sibling userspace BSFS through the same workload
sources and its native API.

The publication runner is destructive. It formats the selected partition
before every measured scenario and repetition. Read the safety section before
running it.

## Build and local checks

The old-BSFS source directory must be provided through `OLD_BSFS_DIR`. It is
only read; its sources are compiled into this repository's build directory.

```sh
make -C bench OLD_BSFS_DIR=/absolute/path/to/bsfs_old
make -C bench OLD_BSFS_DIR=/absolute/path/to/bsfs_old check
```

For runs that do not select `old-bsfs`, build only the POSIX runner without an
old-BSFS checkout:

```sh
make -C bench bin/bench-posix
```

The build creates:

- `bench/bin/bench-posix`
- `bench/bin/bench-old`
- `bench/bin/mkfs-old-bsfs`

Both runners use `-O2`. The external old-BSFS sources retain warnings because
new compilers diagnose existing code that is outside this repository; the
shared benchmark sources are compiled with warnings as errors.

## Dry run

A dry run performs discovery and renders the entire command schedule without
creating the image, attaching a loop device, formatting, mounting, changing a
governor, or dropping caches:

```sh
python3 -m bench.orchestration.run \
  --config bench/config/quick.toml \
  --loop-image /tmp/bsfs-bench.img \
  --filesystems old-bsfs ext4 btrfs \
  --dry-run
```

If the loop image already exists, its resolved path must also be passed through
`--confirm`. Actual loop and physical runs require root.

## Development loop run

Loop results are always marked non-publication:

```sh
sudo python3 -m bench.orchestration.run \
  --config bench/config/quick.toml \
  --loop-image /tmp/bsfs-bench.img \
  --filesystems bsfs old-bsfs ext4 btrfs
```

When launched through `sudo`, the completed result directory is handed back to
the invoking user, so analysis can run without `sudo`. The mountpoint under
`mnt/` is skipped while it is still mounted, and a loop image passed with
`--loop-image` lives outside the result directory and stays root-owned.

Before an actual run, build the C runner(s) required by the selected
filesystems. The launcher verifies they are executable before it creates or
attaches the loop image; it does not build them automatically, so rebuild after
editing anything under `bench/src/`. Every run records the size, mtime, and
SHA-256 of each built artifact under `built_artifacts` in `manifest.json`, so a
stale binary is detectable after the fact.

When `bsfs` is selected, the launcher does build `bsfs2.ko`, removes any
stale `bsfs2` module, and verifies the new module can load on the running
kernel before it creates a result directory or touches the benchmark target.
If this fails (for example, `.env` points `KDIR` at a different kernel), the
launcher exits with setup status 2 and reports the running kernel and module
vermagic. Set `KDIR` to the running kernel's build tree, rebuild, and retry.

The quick profile runs reduced versions of every common, recovery, and
one-/four-worker scenario. Specify only installed filesystems; `zfs` and
`zfs-4k` require working `zpool`, `zfs`, and the matching kernel module. A
filesystem whose tools are missing is recorded as a failure and the run exits
nonzero.

## Progress output

Every run reports what it is doing on **stderr**, so `stdout` stays empty and
the run can be piped or `tee`d without losing the log. The header states the
config, the pinned CPUs, the result directory, and the full plan: how many
filesystems, mkfs cycles, and measured phases the matrix will execute. After
that, each fresh filesystem prints its conditioning, mkfs, and mount, and each
phase prints a counter, its wall time, and the headline metric read back from
`raw/results.jsonl`:

```
[fs 1/2 bsfs] common/bulk r0  eta 9m12s
  condition device (no-discard)                   0.2s
  mkfs bsfs                                       0.9s
  mount (noatime)                                 0.1s
  [ 1/62   2%] bulk/prepare               w1     14.2s   312.4 MiB/s
  cache: unmount + drop_caches + mount            0.6s
  [ 2/62   3%] bulk/cold-read             w1     21.7s   198.1 MiB/s  p99   1.40ms
  [ 9/62  15%] metadata/stat-warm         w1      2.3s  555.5k ops/s  p99    2.5us
  unmount                                         0.3s
  unit total (6 phases)                          38.1s
```

Every line shares one column grid, so the durations of the mkfs cycle's own
steps line up with the measured phases', and the throughput and p99 cells stack
whatever unit they end up in.

Each mkfs cycle closes with `unit total`, the real time it took including its
conditioning, mkfs, mount and unmount, and each section closes with its own
total, so the ETA can be read against what actually happened:

```
preflight
  ...
  preflight total                                1m04s

matrix
  ...
  matrix total                                  42m11s
```

Failures are printed as they happen, with the path to the failing command's
captured stderr, instead of only surfacing in `manifest.json`. A closing
summary gives the total wall time, per-filesystem phase and failure counts,
the kernel-log finding count, and the `analyze` command to run next.

| flag | effect |
| --- | --- |
| *(default)* | phase-level progress, device steps, cache transitions, unit and section totals, failures |
| `-v`, `--verbose` | additionally every subprocess with its duration, and the per-filesystem plan breakdown |
| `-q`, `--quiet` | only the run title, failures, and the closing summary |
| `--no-color` | never emit ANSI colour (also honoured via `NO_COLOR`, and colour is off automatically when stderr is not a terminal) |

Reporting cannot perturb a measurement: it writes only from the main thread
between subprocess calls, uses no background threads or timers, and reads
results back only after the measured process has exited. Each command's wall
time is also recorded as `duration_s` in `commands.jsonl`.

## Physical publication run

Publication mode accepts only a partition symlink under
`/dev/disk/by-id/` ending in `-partN`. Resolve it before confirmation:

```sh
target=/dev/disk/by-id/REPLACE-ME-part1
resolved=$(realpath "$target")

sudo python3 -m bench.orchestration.run \
  --config bench/config/publication.toml \
  --device "$target" \
  --confirm "$resolved"
```

The safety layer rejects whole disks, mounted targets, targets with holders,
device-mapper members, active swap, and any target whose parent disk also
backs a mounted filesystem. That last rule is deliberately broad rather than a
list of protected paths: a list cannot know where a given host keeps `/efi`,
`/var` or `/usr`, and the cost of guessing wrong is a wiped partition. Use a
disk with nothing else mounted on it, or pass `--allow-shared-disk`.

Publication partitions must be 4–16 GiB unless `--allow-size-outside-range` is
supplied. Both overrides are recorded in the manifest.

Before measured publication work, the runner:

1. Builds the current module, formatter, and both benchmark runners.
2. Runs smoke, persistence, copy-on-write, crash, ENOSPC-crash, fuzz, alias,
   and recovery correctness gates.
3. Runs a reduced untimed validation matrix.
4. Records the fixed cyclic filesystem schedule and exact commands.

`--skip-correctness-gates` is explicit and permanently marks the run
non-publication.

## Outputs

Each run directory contains:

- `manifest.json`: host, kernel, repositories, device, tools, affinity,
  configuration, correctness gates, failures, and the fixed filesystem
  schedule.
- `commands.jsonl` and `logs/`: exact argv vectors and command output.
- `raw/results.jsonl`: one schema-versioned object per benchmark phase.
- `raw/artifacts/`: ordered binary latency samples and recovery timelines.
- `raw/kernel.log`: kernel messages emitted during the measured run.

Failures are retained in JSONL and logs. The runner continues with later
filesystem/scenario combinations after a measured workload failure and returns
nonzero after cleanup.

Generate publication tables and a consolidated HTML report with:

```sh
python3 -m bench.orchestration.analyze bench/results/RUN_ID
```

The analyzer uses Python's standard library. It writes median/min/max, mean,
standard deviation, device write amplification, failed phases, and concurrency
scaling as CSV files under `derived/`. It also writes `derived/report.html`, a
single self-contained, locally viewable report with inline styling,
interactive SVG comparisons, run provenance, failures, scaling, and the
complete summary table. It never creates a combined filesystem score.

## How throughput is reported

Two rates are recorded per phase, and they are not interchangeable:

- `durable_throughput_mib_s` / `durable_iops` divide by `durable_total_ns`,
  which includes the phase's final flush. **These are the comparable numbers**
  and the ones the charts plot.
- `throughput_mib_s` / `iops` divide by the operation path alone. For buffered
  writes that is page-cache ingest rate, not filesystem throughput. It is kept
  in `summary.csv` because the gap between the two is itself interesting, but
  ranking filesystems on it ranks how much each defers writeback, not how fast
  each is.

`write_amplification_*` is only computed for phases that contain a flush
(`has_durable_window` is true). Without one the device counters describe
whatever writeback happened to land during the phase.

## Comparability caveats

- Metadata lookups run twice: `lookup-stat-cold` after a cache drop and
  `lookup-stat-warm` immediately after. The warm pass is a dentry-cache
  measurement and is near-identical for every kernel filesystem; only the cold
  pass says anything about on-disk layout.
- btrfs is formatted `-m single`. The `mkfs.btrfs` default on a single device
  is DUP metadata, which doubles every metadata write; that would show up as
  btrfs write amplification for reasons unrelated to its design.
- `zfs` runs at ZFS's own 128 KiB default recordsize, so `random-overwrite-4k`
  is a 128 KiB read-modify-write there. `zfs-4k` is the same pool at
  `recordsize=4K`, matched to the 4 KiB block size the other filesystems are
  formatted with. Run both to separate "ZFS defaults" from "ZFS at parity".
- Every result is a fresh-filesystem result: each repetition reformats and
  re-conditions the device. There is no aging and no fragmentation.
- `old-bsfs` is a userspace filesystem driving a raw block device through its
  own API, with no VFS, no dentry cache, and no page-cache-backed lookups. It
  does not take part in the concurrency matrix.

## CPU selection

`common_cpu` and `concurrency_cpus` default to `"auto"`, which picks CPUs on
distinct physical cores and avoids CPU 0 (the usual default IRQ target). An
explicit list is still honoured but is rejected if two entries are SMT
siblings, since four workers on two cores is not four-way parallelism. The
resolved selection, the sibling map, `lscpu -p` and `/proc/interrupts` are all
recorded in the manifest.

## Manual runner interface

The orchestration layer invokes restartable phases so cache transitions remain
outside the C benchmark:

```sh
bench/bin/bench-posix \
  --root /mnt/bsfs-bench \
  --scenario bulk \
  --phase prepare \
  --profile quick \
  --seed 1234 \
  --results /tmp/results.jsonl

bench/bin/bench-old \
  --device /dev/loop0 \
  --scenario bulk \
  --phase prepare \
  --profile quick \
  --seed 1234 \
  --results /tmp/results.jsonl
```

Use `--help` to list result labels, affinity, backing-device statistics, and
artifact options.
