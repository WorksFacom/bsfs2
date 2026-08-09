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
the invoking user, so analysis can run without `sudo`.

Before an actual run, build the C runner(s) required by the selected
filesystems. The launcher verifies they are executable before it creates or
attaches the loop image; it does not build them automatically.

The quick profile runs reduced versions of every common, recovery, and
one-/four-worker scenario. Specify only installed filesystems; `zfs` and
`zfs-4k` require working `zpool`, `zfs`, and the matching kernel module. A
filesystem whose tools are missing is recorded as a failure and the run exits
nonzero.

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

Generate publication tables and PNG/SVG charts with:

```sh
python3 -m bench.orchestration.analyze bench/results/RUN_ID
```

The analyzer uses Python's standard library and Matplotlib. It writes
median/min/max, mean, standard deviation, device write amplification, failed
phases, and concurrency scaling under `derived/`. It never creates a combined
filesystem score.

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
