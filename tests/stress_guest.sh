#!/bin/sh
# Stress test for the directory tree: creates enough files to force splits
# (t=8 => 15 dentries per node, so the 16th file already splits the root), then
# checks lookup/listing, persistence across remount, and removal (merge/borrow).
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_stress.img
MNT=/tmp/bsfs_stress_mnt
LOOP=""
N="${STRESS_N:-200}"

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

dd if=/dev/zero of="$IMG" bs=1M count=32 2>/dev/null || fail "dd failed"
MKFS_OUT=$("$REPO/mkfs.bsfs" "$IMG" 2>&1) || fail "mkfs.bsfs failed: $MKFS_OUT"
insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"
LOOP=$(losetup -f) || fail "losetup -f failed"
losetup "$LOOP" "$IMG" || fail "losetup $LOOP failed"
mkdir -p "$MNT"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "mount failed"

# 1. Create N files -> forces successive splits of the dentry tree.
i=1
while [ "$i" -le "$N" ]; do
  touch "$MNT/f$i" || fail "touch f$i failed (split regression?)"
  i=$((i + 1))
done

# 2. Every one of them must be listed and reachable through lookup.
ls "$MNT" > /tmp/stress_ls || fail "ls failed"
COUNT=$(grep -c '^f[0-9]*$' /tmp/stress_ls)
[ "$COUNT" -eq "$N" ] || fail "ls listed $COUNT of $N files (incomplete leaf traversal?)"

for i in 1 "$((N / 2))" "$N"; do
  stat "$MNT/f$i" >/dev/null 2>&1 || fail "stat f$i failed (match taken from an internal node?)"
done

# 3. Contents: write to one of the files and read it back.
printf 'content-%s\n' "$N" > "$MNT/f1" || fail "write f1 failed"
sync
OUT=$(cat "$MNT/f1") || fail "cat f1 failed"
[ "$OUT" = "content-$N" ] || fail "read back of f1 diverged: '$OUT'"

# 4. Persistence: everything must survive the remount.
umount "$MNT" || fail "umount failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed"
ls "$MNT" > /tmp/stress_ls2 || fail "ls after remount failed"
COUNT=$(grep -c '^f[0-9]*$' /tmp/stress_ls2)
[ "$COUNT" -eq "$N" ] || fail "after remount only $COUNT of $N files are left (COW root not persisted?)"
OUT=$(cat "$MNT/f1") || fail "cat f1 after remount failed"
[ "$OUT" = "content-$N" ] || fail "f1 diverged after remount: '$OUT'"

# 5. Remove half of the files -> exercises merge/borrow.
i=1
while [ "$i" -le "$N" ]; do
  rm "$MNT/f$i" || fail "rm f$i failed (merge/borrow regression?)"
  i=$((i + 2))
done

ls "$MNT" > /tmp/stress_ls3 || fail "ls after rm failed"
REMAIN=$(grep -c '^f[0-9]*$' /tmp/stress_ls3)
EXPECT=$((N / 2))
[ "$REMAIN" -eq "$EXPECT" ] || fail "after removal $REMAIN are left, expected $EXPECT"
grep -q '^f2$' /tmp/stress_ls3 || fail "f2 vanished without being removed"
grep -q '^f1$' /tmp/stress_ls3 && fail "f1 still listed after rm"

# 6. And the result of the removals must persist too.
umount "$MNT" || fail "umount 2 failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount 2 failed"
ls "$MNT" > /tmp/stress_ls4 || fail "ls after remount 2 failed"
REMAIN=$(grep -c '^f[0-9]*$' /tmp/stress_ls4)
[ "$REMAIN" -eq "$EXPECT" ] || fail "after remount 2 $REMAIN are left, expected $EXPECT"

echo SMOKE_OK
