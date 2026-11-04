# BSFS+

BSFS+ is an educational Linux filesystem module, `bsfs2.ko`, that registers the type `bsfs`. Every index on disk is a B+tree, every change to a tree is copy-on-write, and deleting a file moves it into a recovery tree instead of freeing its blocks. Two userspace tools come with it. `mkfs.bsfs` formats an image or a block device, and `recovery.bsfs` lists and restores deleted files from an unmounted filesystem.

It started as a kernel port of BSFS, an userspace filesystem with an interactive browser. The original's documentation is [here](https://facom.ufms.br/~brivaldo/bsfs/) and describes that version, not this one.

## Requirements

- Linux with the build tree of the kernel you'll load the module into. Developed initially on 6.18, updated later to 7.2 (as it stands, it doesn't work on 6.18 anymore).
- `gcc` and `make`
- root, for `insmod` and `mount`
- QEMU, only for the test suite

## Building

```sh
make
```

This builds `bsfs2.ko`, `mkfs.bsfs` and `recovery.bsfs` in the project root. It uses the running kernel's tree at `/lib/modules/$(uname -r)/build`. To build against another tree, copy `local.mk.example` to `local.mk` and set `KDIR` there.

`BSFS_ALLOC` picks the block allocator compiled into the module. Exactly one goes in, and `make clean` isn't needed to switch.

```sh
make                                     # linear first-fit scan, the default
make BSFS_ALLOC=heap                     # min-heap of freed blocks
make BSFS_ALLOC=twolevel                 # free count per 96-block bucket
make BSFS_ALLOC=heap BSFS_ALLOC_STATS=y  # plus allocation counters in dmesg at unmount
```

## Usage

Format an image, load the module and mount it:

```sh
dd if=/dev/zero of=bsfs.img bs=1M count=64
./mkfs.bsfs bsfs.img
sudo insmod bsfs2.ko
sudo mkdir -p /mnt/bsfs
sudo mount -t bsfs -o loop bsfs.img /mnt/bsfs
```

`mkfs.bsfs` takes a block device too, such as `/dev/sdaX`. It refuses a target that is mounted, and it rounds the size down to a multiple of 4096 bytes.

Deleted files stay on disk as recovery snapshots until the filesystem runs short of space. Unmount, then use `recovery.bsfs` on the image:

```sh
sudo umount /mnt/bsfs
./recovery.bsfs bsfs.img list            # every snapshot, or list <name> for one name
./recovery.bsfs bsfs.img show 3          # all fields of snapshot 3
./recovery.bsfs bsfs.img stats           # count, total size, oldest and newest
./recovery.bsfs bsfs.img restore 3       # back into its original directory
./recovery.bsfs bsfs.img restore 3 /docs # or into another one, by absolute path
```

Restore handles regular files only.

## Tests and benchmarks

The tests boot a QEMU guest with the kernel in `KDIR` and run a script from `tests/` inside it. Build the initramfs once with `tools/mkinitramfs.sh`, then:

```sh
./tests/smoke.sh                        # tests/smoke_guest.sh
./tests/smoke.sh tests/stress_guest.sh  # any other guest script
```

The benchmark harness is in `bench/`. `make bench` builds it, and `bench/README.md` explains how to run it.

## Documentation

`make docs` generates the API documentation with Doxygen into `doc/docs/html`. See `doc/README.md`.
