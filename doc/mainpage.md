# BSFS+

BSFS+ is a Linux filesystem module. It builds as `bsfs2.ko` and registers the type `bsfs`. Every index on disk is a B+tree, every change to a tree is copy-on-write, and deleting a file moves it into a recovery tree instead of freeing its blocks. Two userspace tools come with it. `mkfs.bsfs` formats an image or a block device, and `recovery.bsfs` lists and restores deleted files while the filesystem is unmounted.

Doxygen builds these pages from the comments in the source. Start with the headers in `include/`, since the `.c` files only document what the headers don't.

## Building and mounting

```sh
make                      # bsfs2.ko, mkfs.bsfs and recovery.bsfs
make BSFS_ALLOC=heap      # same, with another block allocator
```

The module builds against `KDIR` from `local.mk`, or against `/lib/modules/$(uname -r)/build` when there is no `local.mk`. It needs a recent kernel. It builds against 7.2, while 6.18 lacks `inode_state_read()` and fails.

```sh
dd if=/dev/zero of=bsfs.img bs=1M count=64
./mkfs.bsfs bsfs.img
sudo insmod bsfs2.ko
sudo mount -t bsfs -o loop bsfs.img /mnt
```

`mkfs.bsfs` also takes a block device. It refuses a target that is mounted, and it rounds the size down to a multiple of 4096 bytes.

## On-disk layout

| Block | Contents |
|---|---|
| 0 | reserved, zeroed |
| 1 | the superblock, bsfs_super_block |
| 2 .. | the bitmap chain, one bitmap_block_t per 32736 blocks |
| `data_block_start` .. | tree nodes and file data |

Everything past the bitmap is a node of one of four kinds of tree. Each node is one btree_node_t, exactly one block, and bsfs_min_degree() picks its fan-out from the entry size.

| Tree | Root | Entry | Bytes | Entries per node |
|---|---|---|---|---|
| inodes | `inode_root` in the superblock | disk_mapped_inode_t | 76 | 51 |
| recovery | `recovery_root` in the superblock | rec_entry_t | 312 | 11 |
| a file's extents | the inode's `btree_root` | bspan_t | 12 | 255 |
| a directory | the inode's `btree_root` | bsfs_dentry | 264 | 15 |

Leaves hold the entries. Internal nodes hold copies of leaf entries as separators. Directories sort by `(hash_filename(name), name)`, so lookups descend the tree. Integers are in host byte order and the structs are not packed. An image therefore only mounts on the ABI that wrote it, which in practice means x86_64. If a struct size changes, the `_Static_assert`s in bsfs_shared.h fail the build.

## Copy-on-write and the commit barrier

A mutation never writes a live node. btree.c clones the root, clones each node on the way down before touching it, and publishes the new root in btree_handle_t::root_block. The caller copies that root into whatever owns the tree: the inode's `btree_root`, or `inode_root` and `recovery_root` in the superblock.

Freeing the old blocks right away would be wrong. Until the new root reaches the disk, the on-disk tree still points at them, and a crash would leave it full of blocks that belong to someone else. They go on a deferred list instead. bsfs_commit_deferred_frees() syncs the block device and the superblock, issues a cache flush, and only then returns them to the bitmap. That happens on `sync(2)` through the `sync_fs` hook, at unmount, and in bsfs_reclaim_space(). A failed `write_inode` since the last commit means some root only exists in memory, so the commit fails and keeps every deferred block. bsfs_reclaim_space() clears that error and writes every inode back before it commits. `fsync(2)` makes one file durable but doesn't release anything.

## Running out of space

Copy-on-write allocates before it frees, so even unlink and truncate need free blocks. bsfs_init_block_counts() keeps a metadata reserve of 1/64 of the data area, capped at #BSFS_META_RESERVE blocks. Each allocation has a priority, bsfs_alloc_prio_t:

| Priority | Used by | May take |
|---|---|---|
| `BSFS_ALLOC_GROW` | create, link, file data | nothing from the reserve |
| `BSFS_ALLOC_WRITEBACK` | `write_inode` | half of the reserve |
| `BSFS_ALLOC_SHRINK` | truncate, unlink, evict, purge | all of it |

Mutating entry points check bsfs_should_reclaim() before they start. Once free blocks drop to twice the reserve and there are deferred blocks or snapshots to get back, bsfs_reclaim_space() writes back dirty inodes and commits. If space is still short it purges up to #BSFS_PURGE_BATCH recovery snapshots and commits again. Create, mkdir, mknod and `write_begin` also retry once after an `ENOSPC`. Per-mount counters for all of this are in `/sys/kernel/debug/bsfs/<dev>/reclaim_stats`, see reclaim_stats.h.

## The recovery tree

unlink, rmdir and a rename over an existing name record the parent and the name in the inode. When the last link goes, bsfs_evict_inode() adds a rec_entry_t that takes over the file's tree, so the data stays where it was. Each snapshot gets `retention_until` 30 days out, but nothing purges on a timer. bsfs_rec_tree_purge() only runs when space is short, and it drops expired snapshots first, then the oldest.

`recovery.bsfs` works on the unmounted image through libbsfs, which shares bsfs_shared.h with the module:

```sh
./recovery.bsfs bsfs.img list [name]
./recovery.bsfs bsfs.img show <id>
./recovery.bsfs bsfs.img stats
./recovery.bsfs bsfs.img restore <id> [/absolute/bsfs/dir]
```

Restore only handles regular files, and it checks every extent against the bitmap before it writes anything. It reuses the original inode number when that number is free. The file goes into the directory given, else its original parent, else the root. The new trees go out first, then a sync, and the superblock is written last.

## Block allocators

The bitmap is the only record of what is allocated. The allocator picks which free block comes next, and exactly one is compiled in. Each one builds its index at mount and writes nothing to disk. The interface is in alloc_strategy.h.

| BSFS_ALLOC | File | How it picks |
|---|---|---|
| `linear`, the default | alloc_linear.c | first-fit scan from `data_block_start` on every allocation |
| `heap` | alloc_heap.c | min-heap of freed blocks plus a cursor over never-used ones. Picks the same block as `linear` |
| `twolevel` | alloc_twolevel.c | a free count per 96-block bucket, one byte per 96 blocks. Finds a bucket with room, then searches it a word at a time |

`make BSFS_ALLOC_STATS=y` adds allocation counters, printed to the kernel log at unmount.

## Source map

| File | What it does |
|---|---|
| bsfs.c | module init, mount, and the superblock operations |
| bsfs.h | in-memory state: bsfs_super_info, bsfs_inode, btree_handle_t |
| bsfs_shared.h | the on-disk format, compiled into the module and the tools |
| btree.h, btree.c | the copy-on-write B+tree |
| alloc.h, alloc.c | allocation, the reserve, deferred frees and reclaim |
| alloc_strategy.h | the allocator interface |
| bitmap.h, bitmap.c | the block bitmap, pinned for the whole mount |
| inode.h, inode.c | the inode tree, inode lifecycle, block mapping and truncate |
| dir.h, dir.c | directory inode and file operations |
| dirent.h, dirent.c | adding, removing and looking up names in a directory tree |
| file.h, file.c | regular file operations and fsync |
| rec_entry.h, rec_entry.c | the recovery tree |
| reclaim_stats.h, reclaim_stats.c | the debugfs counters |
| libbsfs.h, libbsfs.c | the tools' block IO, in-memory bitmap and copy-on-write tree |
| mkfs.c, recovery.c | `mkfs.bsfs` and `recovery.bsfs` |

## Limits

- Hard links and symlinks aren't implemented. `bsfs_set_inode()` accepts a symlink inode, but its operations table is empty, so it can't be followed.
- Special files can be created, but their device number isn't stored, so it is gone after the inode leaves the cache.
- `i_blocks` and `block_count` are always 0.
- Files stop at 2 GiB - 1, the VFS default `s_maxbytes`. Block numbers are 32-bit, which caps a filesystem near 16 TiB.
- The on-disk format is host-endian and unpacked, see above.
