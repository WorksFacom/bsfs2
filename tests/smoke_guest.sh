#!/bin/sh
# Smoke test run inside the QEMU guest against the freshly built bsfs2.ko.
# Exercises the search paths in src/btree.c
#   - inode tree search: mount succeeds -> root inode was found.
#   - dir tree search  : stat/ls on entries in the root dir.
#   - bspan search_le  : reading file contents back.
#
# Prints exactly one line "SMOKE_OK" on success, "SMOKE_FAIL: <reason>" on failure.
# The host runner (tests/smoke.sh) greps that line to decide pass/fail.
#
# Expects to be executed from the 9p-mounted host share, so $(dirname $0)/..
# points at the repo root where bsfs2.ko and mkfs.bsfs live.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_smoke.img
MNT=/tmp/bsfs_smoke_mnt
LOOP=""

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

# 1. Build a fresh image.
dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null || fail "dd failed"
MKFS_OUT=$("$REPO/mkfs.bsfs" "$IMG" 2>&1) || fail "mkfs.bsfs failed: $MKFS_OUT"

# 2. Load the module.
insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"

# 3. Attach image to a loop device.
# busybox does not understand "losetup -f --show IMG"; -f only prints the
# first free device.
LOOP=$(losetup -f) || fail "losetup -f failed"
losetup "$LOOP" "$IMG" || fail "losetup $LOOP failed"

# 4. Mount — exercises the inode B+ tree search (root inode lookup).
mkdir -p "$MNT"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "mount failed (inode tree search regression?)"

# 5. Exercise dir tree search: create some entries and re-look them up.
#    A fresh image starts empty; touch/mkdir + ls/stat cover the dentry
#    B+ path (btree_search_entry).
touch "$MNT/a" 2>&1 || fail "touch a failed"
touch "$MNT/b" 2>&1 || fail "touch b failed"
mkdir "$MNT/d" 2>&1 || fail "mkdir d failed"

ls "$MNT" >/tmp/smoke_ls 2>&1 || fail "ls failed"
grep -q '^a$' /tmp/smoke_ls || fail "ls did not list 'a' (dir search regression?)"
grep -q '^b$' /tmp/smoke_ls || fail "ls did not list 'b'"
grep -q '^d$' /tmp/smoke_ls || fail "ls did not list 'd'"

stat "$MNT/a" >/dev/null 2>&1 || fail "stat a failed (dentry lookup regression?)"
stat "$MNT/b" >/dev/null 2>&1 || fail "stat b failed"
stat "$MNT/d" >/dev/null 2>&1 || fail "stat d failed"

# 6. Exercise bspan search: write a small file, then read it back.
printf 'hello-bsfs\n' >"$MNT/a" 2>&1 || fail "write a failed"
sync
OUT=$(cat "$MNT/a" 2>&1) || fail "cat a failed (bspan_search_le regression?)"
[ "$OUT" = "hello-bsfs" ] || fail "read mismatch: got '$OUT'"

# 7. Miss-path: a non-existent entry must return ENOENT cleanly, not oops.
stat "$MNT/does_not_exist" 2>/dev/null && fail "stat on missing entry unexpectedly succeeded"

# 8. Remount to make sure persistence didn't regress.
umount "$MNT" || fail "umount failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed"
ls "$MNT" >/tmp/smoke_ls2 2>&1 || fail "ls after remount failed"
grep -q '^a$' /tmp/smoke_ls2 || fail "'a' lost after remount"

echo SMOKE_OK
