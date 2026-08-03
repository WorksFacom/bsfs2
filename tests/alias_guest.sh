#!/bin/sh
# Buffer aliasing test: a block that was a btree node must not overwrite the
# file data it becomes.
#
# Freed metadata blocks keep a dirty buffer_head in the block device's page
# cache - merges, root collapses and rolled-back transactions all leave one
# behind. File data goes through the inode's own mapping instead, so once the
# allocator hands such a block to a file there are two dirty aliases for the
# same disk block and whichever writeback runs last wins. bsfs_get_block() now
# calls clean_bdev_aliases() to drop the stale one.
#
# The shape here is: churn the directory tree hard enough to free a lot of
# metadata, commit so those blocks become allocatable again, then immediately
# consume them with data of known content.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_alias.img
MNT=/tmp/bsfs_alias_mnt
LOOP=""
ROUNDS="${ALIAS_ROUNDS:-6}"

cleanup() {
  umount "$MNT" 2>/dev/null
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  rmmod bsfs2 2>/dev/null
  rm -rf "$MNT" "$IMG" /tmp/alias_want
}
trap cleanup EXIT

fail() {
  echo "SMOKE_FAIL: $*"
  exit 1
}

dd if=/dev/zero of="$IMG" bs=1M count=32 2>/dev/null || fail "dd failed"
MKFS_OUT=$("$REPO/mkfs.bsfs" "$IMG" 2>&1) || fail "mkfs.bsfs failed: $MKFS_OUT"
insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"
LOOP=$(losetup -f) || fail "losetup -f failed"
losetup "$LOOP" "$IMG" || fail "losetup $LOOP failed"
mkdir -p "$MNT"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "mount failed"

# A distinctive payload: if a stale metadata buffer lands on top of it, the
# comparison fails on content, not just on length.
dd if=/dev/urandom of=/tmp/alias_want bs=4096 count=32 2>/dev/null || fail "dd payload failed"
WANT=$(md5sum </tmp/alias_want | cut -d' ' -f1)

r=1
while [ "$r" -le "$ROUNDS" ]; do
  mkdir "$MNT/d$r" || fail "round $r: mkdir failed"

  # Grow the dentry tree past a few splits...
  i=1
  while [ "$i" -le 120 ]; do
    touch "$MNT/d$r/f$i" || fail "round $r: touch f$i failed"
    i=$((i + 1))
  done

  # ...then collapse it, which merges nodes and frees their blocks while their
  # buffers are still dirty.
  i=1
  while [ "$i" -le 120 ]; do
    rm "$MNT/d$r/f$i" || fail "round $r: rm f$i failed"
    i=$((i + 1))
  done
  rmdir "$MNT/d$r" || fail "round $r: rmdir failed"

  # Commit, so exactly those blocks go back to the allocator...
  sync

  # ...and grab them straight back as file data.
  cp /tmp/alias_want "$MNT/data$r" || fail "round $r: write data failed"
  sync

  GOT=$(md5sum <"$MNT/data$r" | cut -d' ' -f1)
  [ "$WANT" = "$GOT" ] || fail "round $r: data$r was corrupted in cache (stale metadata buffer?)"

  r=$((r + 1))
done

# The page cache can hide the damage: the stale buffer only lands on disk at
# writeback. Drop everything by remounting and read it all back.
umount "$MNT" || fail "umount failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed"

r=1
while [ "$r" -le "$ROUNDS" ]; do
  GOT=$(md5sum <"$MNT/data$r" | cut -d' ' -f1)
  [ "$WANT" = "$GOT" ] || fail "data$r diverged on disk (stale metadata buffer overwrote it)"
  r=$((r + 1))
done

echo SMOKE_OK
