#!/bin/sh
# Mount-time validation test: a corrupt superblock or bitmap chain must be
# refused, not followed.
#
# load_bitmap() used to walk the on-disk next_block chain until it happened to
# see INVALID_BLOCK, with no bound and no check on sb_bread(), writing one
# buffer_head pointer per hop into an array sized from a *different* on-disk
# field. A short block_bitmap_total, or a chain that does not terminate, walked
# straight off the end of it. The bit accessors index the same array from a
# block number, so a superblock claiming fewer bitmap blocks than the filesystem
# has blocks is the same bug by another route.
#
# Every case here must fail the mount cleanly; the test passes if the module is
# still alive and a good image still mounts afterwards.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_fuzz.img
MNT=/tmp/bsfs_fuzz_mnt
LOOP=""

# offsets in the on-disk layout (see include/bsfs_shared.h)
#   superblock is block 1; block_bitmap_total is its 7th u32-ish field
SB_BITMAP_TOTAL=$((4096 + 24))
#   bitmap blocks start at block 2; next_block is the last u32 of each
BITMAP0_NEXT=$((2 * 4096 + 4092))

cleanup() {
  umount "$MNT" 2>/dev/null
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  rmmod bsfs2 2>/dev/null
  rm -rf "$MNT" "$IMG"
}
trap cleanup EXIT

fail() {
  echo "SMOKE_FAIL: $*"
  exit 1
}

# writes 4 little-endian bytes at a byte offset in the image
poke32() {
  printf "$2" | dd of="$IMG" bs=1 seek="$1" conv=notrunc 2>/dev/null || fail "poke at $1 failed"
}

fresh_image() {
  # sparse: mkfs only writes the metadata, so 256 MB costs almost nothing. Big
  # enough that the bitmap needs more than one block, which is what makes the
  # chain walk reachable at all.
  rm -f "$IMG"
  truncate -s 256M "$IMG" || fail "truncate failed"
  "$REPO/mkfs.bsfs" "$IMG" >/dev/null 2>&1 || fail "mkfs.bsfs failed"
}

# mounts and expects failure; leaves nothing mounted either way
expect_mount_fails() {
  losetup "$LOOP" "$IMG" || fail "losetup failed ($1)"

  if mount -t bsfs "$LOOP" "$MNT" 2>/dev/null; then
    umount "$MNT" 2>/dev/null
    losetup -d "$LOOP" 2>/dev/null
    fail "$1: mount succeeded on a corrupt image"
  fi

  losetup -d "$LOOP" 2>/dev/null
}

insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"
mkdir -p "$MNT"
LOOP=$(losetup -f) || fail "losetup -f failed"

# 0. Sanity: the pristine image mounts, so the failures below mean something.
fresh_image
losetup "$LOOP" "$IMG" || fail "losetup failed (baseline)"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "baseline image did not mount"
echo "fuzz: baseline free=$(df -Pk "$MNT" | awk 'NR == 2 { print $4 }')K"
umount "$MNT" || fail "baseline umount failed"
losetup -d "$LOOP" || fail "baseline losetup -d failed"

# 1. block_bitmap_total = 0. Nothing to index at all.
fresh_image
poke32 "$SB_BITMAP_TOTAL" '\000\000\000\000'
expect_mount_fails "zero bitmap blocks"

# 2. block_bitmap_total = 1, which is far too few to describe 65536 blocks.
#    This is the one that used to corrupt the heap on the first allocation.
fresh_image
poke32 "$SB_BITMAP_TOTAL" '\001\000\000\000'
expect_mount_fails "short bitmap count"

# 3. A next_block pointing outside the filesystem.
fresh_image
poke32 "$BITMAP0_NEXT" '\376\377\377\377'
expect_mount_fails "out-of-range bitmap chain"

# 4. A next_block pointing at block 0, which is not a bitmap block.
fresh_image
poke32 "$BITMAP0_NEXT" '\000\000\000\000'
expect_mount_fails "bitmap chain through block 0"

# 5. The module survived all of that and still mounts a good image and works.
fresh_image
losetup "$LOOP" "$IMG" || fail "losetup failed (recovery)"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "a good image no longer mounts after the fuzz"

i=1
while [ "$i" -le 20 ]; do
  echo "ok-$i" >"$MNT/f$i" || fail "write f$i failed after the fuzz"
  i=$((i + 1))
done
sync

umount "$MNT" || fail "umount failed (recovery)"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed (recovery)"
OUT=$(cat "$MNT/f20") || fail "cat f20 failed"
[ "$OUT" = "ok-20" ] || fail "f20 diverged: '$OUT'"

echo SMOKE_OK
