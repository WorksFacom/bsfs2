#!/bin/sh
# Verifies that reclaim instrumentation is mount-scoped, read-only and updated
# by a real automatic recovery purge.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_reclaim_stats.img
MNT=/tmp/bsfs_reclaim_stats_mnt
LOOP=""
DEBUG_MOUNTED=0

cleanup() {
  umount "$MNT" 2>/dev/null
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  rmmod bsfs2 2>/dev/null
  [ "$DEBUG_MOUNTED" -eq 1 ] && umount /sys/kernel/debug 2>/dev/null
  rm -rf "$MNT" "$IMG"
}
trap cleanup EXIT

fail() {
  echo "SMOKE_FAIL: $*"
  exit 1
}

value() {
  awk -v key="$1" '$1 == key { print $2 }' "$STATS"
}

fill() {
  _path=$1
  _limit=$2
  _n=0
  while [ "$_n" -lt "$_limit" ]; do
    dd if=/dev/zero of="$_path" bs=4096 count=1 seek="$_n" conv=notrunc 2>/dev/null || break
    _n=$((_n + 1))
  done
  echo "$_n"
}

mkdir -p /sys/kernel/debug
if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
  mount -t debugfs debugfs /sys/kernel/debug || fail "cannot mount debugfs"
  DEBUG_MOUNTED=1
fi

dd if=/dev/zero of="$IMG" bs=1M count=32 2>/dev/null || fail "dd failed"
"$REPO/mkfs.bsfs" "$IMG" >/dev/null 2>&1 || fail "mkfs failed"
insmod "$REPO/bsfs2.ko" || fail "insmod failed"
LOOP=$(losetup -f) || fail "no loop device"
losetup "$LOOP" "$IMG" || fail "losetup failed"
mkdir -p "$MNT"
mount -t bsfs "$LOOP" "$MNT" || fail "mount failed"

STATS="/sys/kernel/debug/bsfs/$(basename "$LOOP")/reclaim_stats"
[ -r "$STATS" ] || fail "stats file missing: $STATS"
[ "$(value attempts)" = 0 ] || fail "attempts did not reset at mount"

WROTE=$(fill "$MNT/first" 8192)
[ "$WROTE" -gt 100 ] || fail "could not fill fresh filesystem"
rm "$MNT/first" || fail "logical deletion failed"
sync
AGAIN=$(fill "$MNT/second" 8192)
[ "$AGAIN" -gt 100 ] || fail "could not refill through reclaim"
sync

[ "$(value attempts)" -gt 0 ] || fail "no reclaim attempt recorded"
[ "$(value successes)" -gt 0 ] || fail "no successful reclaim recorded"
[ "$(value entries_purged)" -gt 0 ] || fail "no purged entry recorded"
[ "$(value blocks_reclaimed)" -gt 0 ] || fail "no reclaimed blocks recorded"
[ "$(value pre_commit_ns)" -gt 0 ] || fail "pre-commit timing missing"
[ "$(value purge_ns)" -gt 0 ] || fail "purge timing missing"
[ "$(value post_commit_ns)" -gt 0 ] || fail "post-commit timing missing"

umount "$MNT" || fail "unmount failed"
[ ! -e "$STATS" ] || fail "mount stats survived unmount"

echo SMOKE_OK
