# BSFS benchmark suite

This suite runs kernel BSFS, Ext4, Btrfs, and ZFS through one POSIX runner. The
sibling userspace BSFS runs the same workload sources through its native API.

The publication runner is destructive. It formats the selected partition
before every measured scenario and repetition. Read the safety section before
running it.

## Build and local checks

Point `OLD_BSFS_DIR` at the old-BSFS source tree. It is only read, and its
objects land in this repository's build directory.

```sh
make -C bench OLD_BSFS_DIR=/absolute/path/to/bsfs_old
make -C bench OLD_BSFS_DIR=/absolute/path/to/bsfs_old check
```

Runs without `old-bsfs` only need the POSIX runner, which builds without an
old-BSFS checkout:

```sh
make -C bench bin/bench-posix
```

The build creates:

- `bench/bin/bench-posix`
- `bench/bin/bench-old`
- `bench/bin/mkfs-old-bsfs`

Both runners use `-O2`. The shared benchmark sources build with `-Werror`. The
old-BSFS sources don't, since newer compilers warn about code that lives outside
this repository.

## Dry run

A dry run does discovery and prints the full command schedule. It never creates
the image, attaches a loop device, formats, mounts, touches the governor, or
drops caches:

```sh
python3 -m bench.orchestration.run \
  --config bench/config/quick.toml \
  --loop-image /tmp/bsfs-bench.img \
  --filesystems old-bsfs ext4 btrfs \
  --dry-run
```

If the loop image already exists, pass its resolved path to `--confirm` too.
Real loop and physical runs need root.

## Development loop run

Loop results are always marked non-publication:

```sh
sudo python3 -m bench.orchestration.run \
  --config bench/config/quick.toml \
  --loop-image /tmp/bsfs-bench.img \
  --filesystems bsfs old-bsfs ext4 btrfs
```

Under `sudo`, the finished result directory is chowned back to the invoking
user, so analysis runs without `sudo`. The mountpoint under `mnt/` is skipped
while still mounted. The `--loop-image` file lives outside the result directory
and stays root-owned.

Build the C runners the selected filesystems need before a real run. The
launcher checks they are executable before it creates or attaches the loop
image, but it won't build them, so rebuild after touching `bench/src/`. Every
run records the size, mtime, and SHA-256 of each built artifact under
`built_artifacts` in `manifest.json`, so a stale binary shows up after the fact.

With a repository-built `bsfs` target selected, the launcher builds `bsfs2.ko`,
unloads any stale `bsfs2`, and checks the new module loads on the running kernel.
All of that happens before it creates a result directory or touches the target.
If it fails (say `local.mk` points `KDIR` at another kernel), the launcher exits
with setup status 2 and prints the running kernel and the module vermagic. Point
`KDIR` at the running kernel's build tree, rebuild, and retry.

## Allocation strategies

The block allocator is picked at compile time. `BSFS_ALLOC` in the top-level
Makefile chooses which `src/alloc_<name>.c` to link. Every choice produces
`bsfs2.ko`, registering the `bsfs` filesystem type. The benchmark discovers
targets from those source files, so adding `src/alloc_foo.c` makes `bsfs-foo`
selectable:

```sh
sudo python3 -m bench.orchestration.run \
  --config bench/config/alloc.toml \
  --loop-image /tmp/bsfs-bench.img
```

`bench/config/alloc.toml` selects `bsfs-linear`, `bsfs-heap`, and
`bsfs-twolevel`. They mix freely with `ext4`, `btrfs`, and the rest, and run the
full matrix including recovery pressure and concurrency. Plain `bsfs` is still
the default build, the same module as `bsfs-linear`. Selecting both benchmarks
one build under two labels, and the launcher warns about it.

Preflight builds each selected strategy once, copies each `.ko` to
`bench/build/modules/`, then loads and unloads it. Nothing compiles between
measured phases, and a strategy that can't build or load fails before the runner
touches a device. Preflight sets `BSFS_ALLOC_STATS=` to compile out the
allocation counters, since they sit on the allocation fast path. It restores the
default build afterward so the checkout's normal artifacts stay usable for
development and the optional QEMU checks.

`manifest.json` records each copied module under `built_artifacts` with its
SHA-256. `filesystem_configuration` records the strategy for each target. The
mount-time `bsfs2: allocator '<name>' on <device>` message in `dmesg` identifies
the module used for a phase.

`tests/alloc_matrix.sh` runs the QEMU suite for every strategy with allocation
counters compiled in.

## Custom BSFS builds

A separately built BSFS can run next to the allocator variants and the other
filesystems. Give it a unique target name, its module, and the matching
formatter:

```sh
sudo python3 -m bench.orchestration.run \
  --config bench/config/alloc.toml \
  --loop-image /tmp/bsfs-bench.img \
  --custom-bsfs bsfs-maple /absolute/vfs_maple/bsfs2.ko \
    /absolute/vfs_maple/mkfs.bsfs \
  --filesystems bsfs-maple bsfs-linear bsfs-heap bsfs-twolevel ext4 btrfs
```

`--custom-bsfs NAME MODULE MKFS` is repeatable. `NAME` must be a lowercase
`bsfs-*` slug that doesn't collide with a built-in target. Registering a target
doesn't select it. List the same name in `--filesystems` or in the TOML
`filesystems` list. Only selected custom targets get staged and probe-loaded.

The formatter is mandatory. A module from another branch may not share this
checkout's on-disk format. `vfs_maple`, for one, changes the superblock and
B-tree layouts, so pairing its module with this checkout's `mkfs.bsfs` would be
a bogus comparison.

Preflight copies each selected pair to `bench/build/custom/NAME/`, checks the
module is named `bsfs2`, probe-loads it, checks it registers the `bsfs`
filesystem type, and unloads it, all before the benchmark target is touched.
The custom target then runs the same common, recovery, concurrency, and
publication-validation matrices as the repository-built targets.

Custom artifacts stay publication eligible when every normal publication check
passes. The publication validation matrix covers them, but the optional QEMU
correctness gates only cover this checkout's default module. The manifest
records both artifact hashes, their original and staged paths, the module
vermagic, and whatever source Git state it can find.

The quick profile runs cut-down versions of every common, recovery, and
one- and four-worker scenario. Only list installed filesystems. `zfs` and
`zfs-4k` need working `zpool`, `zfs`, and the matching kernel module. A
filesystem with missing tools is recorded as a failure and the run exits
nonzero.

## Progress output

Progress goes to stderr, so stdout stays empty and the run can be piped or
`tee`d without losing the log. The header lists the config, the pinned CPUs, the
result directory, and the plan (how many filesystems, mkfs cycles, and measured
phases). Then each fresh filesystem prints its conditioning, mkfs, and mount,
and each phase prints a counter, its wall time, and the headline metric read
back from `raw/results.jsonl`:

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

Columns are fixed width, so durations line up, and so do throughput and p99
values in different units.

Each mkfs cycle ends with `unit total`, covering conditioning, mkfs, mount, and
unmount. Each section ends with its own total, so the ETA tracks real elapsed
work:

```
preflight
  ...
  preflight total                                1m04s

matrix
  ...
  matrix total                                  42m11s
```

On failure the runner prints the path to the command's captured stderr rather
than burying it in `manifest.json`. The closing summary lists total wall time,
phase and failure counts per filesystem, kernel-log findings, and the `analyze`
command.

| flag | effect |
| --- | --- |
| *(default)* | phase-level progress, device steps, cache transitions, unit and section totals, failures |
| `-v`, `--verbose` | every subprocess with its duration, plus the per-filesystem plan breakdown |
| `-q`, `--quiet` | only the run title, failures, and the closing summary |
| `--no-color` | never emit ANSI colour (also honoured via `NO_COLOR`, and colour is off automatically when stderr is not a terminal) |

The reporter writes from the main thread between subprocess calls. No
background threads, no timers, and it reads results only after the measured
process exits. `commands.jsonl` records each command's wall time as
`duration_s`.

## Physical publication run

Publication mode only accepts a partition symlink under `/dev/disk/by-id/`
ending in `-partN`. Resolve it before confirming:

```sh
target=/dev/disk/by-id/REPLACE-ME-part1
resolved=$(realpath "$target")

sudo python3 -m bench.orchestration.run \
  --config bench/config/publication.toml \
  --device "$target" \
  --confirm "$resolved"
```

The safety layer rejects whole disks, mounted targets, targets with holders,
device-mapper members, active swap, and any target whose parent disk also backs
a mounted filesystem. That last rule is broad on purpose. A list of protected
paths can't know where a host keeps `/efi`, `/var`, or `/usr`, and a wrong guess
wipes a partition. Use a disk with nothing else mounted, or pass
`--allow-shared-disk`.

Publication partitions must be 4 to 16 GiB unless you pass
`--allow-size-outside-range`. The manifest records both overrides.

Before any measured publication work, the runner builds the current module,
formatter, and both benchmark runners, runs a reduced untimed validation matrix,
and records the fixed cyclic filesystem schedule and the exact commands.

`--run-correctness-gates` adds the QEMU smoke, persistence, copy-on-write,
crash, ENOSPC-crash, fuzz, alias, and recovery checks. They are off by default
because they need `KDIR` pointing at a bootable kernel build that matches the
module under test. The manifest records whether they were requested and how they
went.

## Outputs

Each run directory contains:

- `manifest.json`: host, kernel, repositories, device, tools, affinity,
  configuration, correctness gates, failures, and the fixed filesystem
  schedule.
- `commands.jsonl` and `logs/`: exact argv vectors and command output.
- `raw/results.jsonl`: one schema-versioned object per benchmark phase.
- `raw/artifacts/`: ordered binary latency samples and recovery timelines.
- `raw/kernel.log`: kernel messages emitted during the measured run.

Failures stay in the JSONL and logs. After a measured workload fails, the
runner moves on to the remaining filesystem and scenario combinations, then
exits nonzero after cleanup.

Generate publication tables and a consolidated HTML report with:

```sh
python3 -m bench.orchestration.analyze bench/results/RUN_ID
```

The analyzer only needs the Python standard library. It writes median/min/max,
mean, standard deviation, device write amplification, failed phases, and
concurrency scaling as CSV under `derived/`. It also writes
`derived/report.html`, one self-contained file with inline styles, run
provenance, failures, scaling, and the full summary table. It never produces a
combined filesystem score. Metric comparisons are horizontal bar charts with one
bar per filesystem, so every filesystem stays on screen no matter how many the
run covered. A toolbar sorts the bars by value (best first for the selected
metric) or alphabetically, and expands or collapses every scenario and phase at
once.

## How throughput is reported

Each phase records two rates, and they are not interchangeable.

- `durable_throughput_mib_s` and `durable_iops` divide by `durable_total_ns`,
  which includes the phase's final flush. These are the comparable numbers and
  the ones the charts plot.
- `throughput_mib_s` and `iops` divide by the operation path alone. For buffered
  writes that is page-cache ingest rate, not filesystem throughput. It stays in
  `summary.csv` because the gap between the two is interesting, but ranking on
  it ranks how much each filesystem defers writeback, not how fast it is.

`write_amplification_*` is only computed for phases that end in a flush
(`has_durable_window` is true). Without one, the device counters just reflect
whatever writeback happened to land during the phase.

## Comparability caveats

- Metadata lookups run twice, `lookup-stat-cold` after a cache drop and
  `lookup-stat-warm` right after. The warm pass measures the dentry cache and
  looks nearly the same on every kernel filesystem. Only the cold pass says
  anything about on-disk layout.
- btrfs is formatted `-m single`. The `mkfs.btrfs` default on a single device
  is DUP metadata, which doubles every metadata write and would show up as btrfs
  write amplification for reasons unrelated to its design.
- `zfs` runs at ZFS's own 128 KiB default recordsize, so `random-overwrite-4k`
  is a 128 KiB read-modify-write there. `zfs-4k` is the same pool at
  `recordsize=4K`, matched to the 4 KiB block size the other filesystems are
  formatted with. Run both to separate "ZFS defaults" from "ZFS at parity".
- Every result is from a fresh filesystem. Each repetition reformats and
  re-conditions the device, so there is no aging and no fragmentation.
- `old-bsfs` is a userspace filesystem driving a raw block device through its
  own API, with no VFS, no dentry cache, and no page-cache-backed lookups. It
  sits out the concurrency matrix. Despite the name it isn't a `bsfs-*`
  allocator variant, it's a different source tree.
- The `bsfs-*` targets differ only in the compiled block allocator. Workloads
  with plenty of free blocks may barely differ. Recovery pressure and
  write-heavy phases are where a difference most likely shows up.

## CPU selection

`common_cpu` and `concurrency_cpus` default to `"auto"`, which picks CPUs on
distinct physical cores and skips CPU 0 (the usual default IRQ target). An
explicit list still works but is rejected if two entries are SMT siblings, since
four workers on two cores isn't four-way parallelism. The manifest records the
resolved selection, the sibling map, `lscpu -p`, and `/proc/interrupts`.

## Manual runner interface

Orchestration calls restartable phases so cache transitions stay out of the C
benchmark:

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

`--help` lists the result label, affinity, backing-device statistics, and
artifact options.
