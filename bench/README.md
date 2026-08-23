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

When a repository-built `bsfs` target is selected, the launcher builds
`bsfs2.ko`, removes any stale `bsfs2` module, and verifies the new module can
load on the running kernel before it creates a result directory or touches the
benchmark target. If this fails (for example, `.env` points `KDIR` at a
different kernel), the launcher exits with setup status 2 and reports the
running kernel and module vermagic. Set `KDIR` to the running kernel's build
tree, rebuild, and retry.

## Allocation strategies

The block allocator is selected at compile time. `BSFS_ALLOC` in the top-level
Makefile chooses the `src/alloc_<name>.c` file to link. Every choice produces
`bsfs2.ko`, which registers the `bsfs` filesystem type. The benchmark discovers
targets from those source files. Add `src/alloc_foo.c` and `bsfs-foo` becomes
selectable:

```sh
sudo python3 -m bench.orchestration.run \
  --config bench/config/alloc.toml \
  --loop-image /tmp/bsfs-bench.img
```

`bench/config/alloc.toml` selects `bsfs-linear`, `bsfs-heap`, and
`bsfs-twolevel`. You can include them in a run with `ext4`, `btrfs`, and the
other filesystems. They run the full matrix, including recovery pressure and
concurrency. Plain `bsfs` still selects the default build, the same module as
`bsfs-linear`. Selecting both benchmarks one build under two labels, and the
launcher warns about it.

Preflight builds every selected strategy once, copies each `.ko` to
`bench/build/modules/`, then loads and unloads it. No compilation runs between
measured phases. A strategy that cannot build or load fails before the runner
touches a device. Preflight sets `BSFS_ALLOC_STATS=` to disable the allocation
counters, which run on the allocation fast path. It restores the default build
afterward because the correctness gates and QEMU scripts boot
`$REPO/bsfs2.ko`.

`manifest.json` records each copied module under `built_artifacts` with its
SHA-256. `filesystem_configuration` records the strategy for each target. The
mount-time `bsfs2: allocator '<name>' on <device>` message in `dmesg` identifies
the module used for a phase.

`tests/alloc_matrix.sh` runs the QEMU suite for every strategy with allocation
counters compiled in.

## Custom BSFS builds

An independently built BSFS implementation can run beside the allocator
variants and the other filesystems. Supply a unique target name, its module,
and the matching formatter:

```sh
sudo python3 -m bench.orchestration.run \
  --config bench/config/alloc.toml \
  --loop-image /tmp/bsfs-bench.img \
  --custom-bsfs bsfs-maple /absolute/vfs_maple/bsfs2.ko \
    /absolute/vfs_maple/mkfs.bsfs \
  --filesystems bsfs-maple bsfs-linear bsfs-heap bsfs-twolevel ext4 btrfs
```

`--custom-bsfs NAME MODULE MKFS` is repeatable. `NAME` must be a lowercase
`bsfs-*` slug and must not collide with a built-in target. Registering a target
does not select it: include the same name in `--filesystems` or in the TOML
`filesystems` list. Only selected custom targets are staged and probe-loaded.

The formatter is mandatory. A module from another branch is not assumed to
share this checkout's on-disk format; for example, `vfs_maple` changes the
superblock and B-tree layouts. Pairing its module with this checkout's
`mkfs.bsfs` would not be a valid comparison.

Preflight copies each selected pair to `bench/build/custom/NAME/`, checks that
the module is named `bsfs2`, probe-loads it, verifies that it registers the
`bsfs` filesystem type, and unloads it before the benchmark target is touched.
The custom target then runs the same common, recovery, concurrency, and
publication-validation matrices as the repository-built BSFS targets.

Custom artifacts remain publication eligible when every normal publication
check succeeds. The manifest explicitly records that the checkout's QEMU
correctness gates cover its default module, while a custom module is covered by
the publication validation matrix only. It also records both artifact hashes,
their original and staged paths, module vermagic, and discoverable source Git
state.

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

The output uses fixed columns. mkfs and phase durations stay aligned, as do
throughput and p99 values with different units.

Each mkfs cycle ends with `unit total`, which includes conditioning, mkfs,
mount, and unmount. Each section also ends with a total, so the ETA reflects
elapsed work:

```
preflight
  ...
  preflight total                                1m04s

matrix
  ...
  matrix total                                  42m11s
```

On failure, the runner prints the path to the command's captured stderr instead
of leaving it only in `manifest.json`. The closing summary lists total wall
time, phase and failure counts for each filesystem, kernel-log findings, and
the `analyze` command.

| flag | effect |
| --- | --- |
| *(default)* | phase-level progress, device steps, cache transitions, unit and section totals, failures |
| `-v`, `--verbose` | every subprocess with its duration, plus the per-filesystem plan breakdown |
| `-q`, `--quiet` | only the run title, failures, and the closing summary |
| `--no-color` | never emit ANSI colour (also honoured via `NO_COLOR`, and colour is off automatically when stderr is not a terminal) |

The reporter writes from the main thread between subprocess calls. It has no
background threads or timers, and reads results only after the measured process
exits. `commands.jsonl` records each command's wall time as `duration_s`.

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
backs a mounted filesystem. The last rule is deliberately broad. A list of
protected paths cannot know where a host keeps `/efi`, `/var`, or `/usr`, and a
wrong guess can wipe a partition. Use a disk with nothing else mounted on it,
or pass `--allow-shared-disk`.

Publication partitions must be 4 to 16 GiB unless `--allow-size-outside-range` is
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
  does not take part in the concurrency matrix. Despite the name it is not a
  `bsfs-*` allocator variant; it is a different source tree.
- The `bsfs-*` targets differ only in the compiled block allocator. Workloads
  with plenty of free blocks may show little difference. Recovery pressure and
  write-heavy phases are the most likely places to show one.

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
