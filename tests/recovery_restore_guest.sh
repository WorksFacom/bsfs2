#!/bin/sh
# offline recovery.bsfs restore integration test. run through tests/smoke.sh.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_recovery_restore.img
MNT=/tmp/bsfs_recovery_restore_mnt
EXPECT=/tmp/bsfs_recovery_restore_expect
LOOP=""

cleanup() {
  umount "$MNT" 2>/dev/null
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  rmmod bsfs2 2>/dev/null
  rm -rf "$MNT" "$IMG" "$EXPECT"
}
trap cleanup EXIT

fail() {
  echo "SMOKE_FAIL: $*"
  exit 1
}

recovery_id() {
  "$REPO/recovery.bsfs" "$IMG" list "$1" 2>/dev/null |
    awk -v name="$1" '$NF == name && $1 ~ /^[0-9]+$/ { print $1; exit }'
}

dd if=/dev/zero of="$IMG" bs=1M count=16 2>/dev/null || fail "image creation failed"
"$REPO/mkfs.bsfs" "$IMG" >/dev/null 2>&1 || fail "mkfs failed"
insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"
LOOP=$(losetup -f) || fail "losetup -f failed"
losetup "$LOOP" "$IMG" || fail "losetup failed"
mkdir -p "$MNT" "$EXPECT"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "initial mount failed"

mkdir "$MNT/original" "$MNT/destination" "$MNT/gone" "$MNT/inodefill" || fail "mkdir failed"

# force the directory and inode trees through leaf splits before restoration.
i=0
while [ "$i" -lt 120 ]; do
  : >"$MNT/inodefill/live-$i" || fail "inode split setup failed"
  if [ $((i % 16)) -eq 15 ]; then sync; fi
  i=$((i + 1))
done
i=0
while [ "$i" -lt 20 ]; do
  : >"$MNT/original/live-$i" || fail "directory split setup failed"
  i=$((i + 1))
done
printf 'offline restore payload\n' >"$MNT/original/basic" || fail "write basic failed"
cp "$MNT/original/basic" "$EXPECT/basic" || fail "save basic expected data failed"
chmod 0640 "$MNT/original/basic" || fail "chmod basic failed"

dd if=/dev/zero of="$MNT/original/multiblock" bs=4096 count=3 2>/dev/null || fail "write multiblock failed"
printf 'tail-marker' | dd of="$MNT/original/multiblock" bs=1 seek=9000 conv=notrunc 2>/dev/null || fail "patch multiblock failed"
cp "$MNT/original/multiblock" "$EXPECT/multiblock" || fail "save multiblock expected data failed"

printf 'old conflict\n' >"$MNT/original/conflict" || fail "write conflict failed"
printf 'root fallback\n' >"$MNT/gone/fallback" || fail "write fallback failed"
sync

rm "$MNT/original/basic" "$MNT/original/multiblock" "$MNT/original/conflict" "$MNT/gone/fallback" || fail "unlink failed"
# force the recovery tree through a split; these empty snapshots are retained.
i=0
while [ "$i" -lt 20 ]; do
  : >"$MNT/original/noise-$i" || fail "recovery split create failed"
  rm "$MNT/original/noise-$i" || fail "recovery split unlink failed"
  i=$((i + 1))
done
printf 'current conflict\n' >"$MNT/original/conflict" || fail "recreate conflict failed"
rmdir "$MNT/gone" || fail "remove fallback parent failed"
sync
MOUNTED_OUT=$("$REPO/recovery.bsfs" "$LOOP" restore 4294967295 2>&1)
echo "$MOUNTED_OUT" | grep -q 'open for exclusive access' || fail "mounted block device was not rejected"
# O_EXCL only covers the block device; the backing file has to be refused through the mount table.
BACKING_OUT=$("$REPO/recovery.bsfs" "$IMG" restore 4294967295 2>&1)
echo "$BACKING_OUT" | grep -q 'mounted through a loop device' || fail "mounted loop backing file was not rejected"
umount "$MNT" || fail "unmount before restore failed"

BASIC_ID=$(recovery_id basic)
MULTI_ID=$(recovery_id multiblock)
CONFLICT_ID=$(recovery_id conflict)
FALLBACK_ID=$(recovery_id fallback)
DIR_ID=$(recovery_id gone)
[ -n "$BASIC_ID" ] && [ -n "$MULTI_ID" ] && [ -n "$CONFLICT_ID" ] && [ -n "$FALLBACK_ID" ] && [ -n "$DIR_ID" ] ||
  fail "could not discover recovery ids"

"$REPO/recovery.bsfs" "$IMG" restore "$BASIC_ID" >/dev/null 2>&1 || fail "default restore failed"
# exercise the block-device path as well as the regular-image path.
"$REPO/recovery.bsfs" "$LOOP" restore "$MULTI_ID" /destination >/dev/null 2>&1 || fail "block-device destination restore failed"
"$REPO/recovery.bsfs" "$IMG" restore "$FALLBACK_ID" >/dev/null 2>&1 || fail "root fallback restore failed"
"$REPO/recovery.bsfs" "$IMG" restore "$CONFLICT_ID" >/dev/null 2>&1 && fail "name collision unexpectedly restored"
"$REPO/recovery.bsfs" "$IMG" restore "$DIR_ID" >/dev/null 2>&1 && fail "directory snapshot unexpectedly restored"

[ -n "$(recovery_id conflict)" ] || fail "failed collision consumed recovery entry"
[ -n "$(recovery_id gone)" ] || fail "failed directory restore consumed recovery entry"
[ -z "$(recovery_id basic)" ] || fail "successful restore left recovery entry"

mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "mount after restore failed"
if ! cmp "$MNT/original/basic" "$EXPECT/basic" >/dev/null 2>&1; then
  stat "$MNT/original/basic" 2>&1
  od -An -tx1 "$MNT/original/basic" 2>&1
  od -An -tx1 "$EXPECT/basic" 2>&1
  fail "basic data mismatch"
fi
if ! cmp "$MNT/destination/multiblock" "$EXPECT/multiblock" >/dev/null 2>&1; then
  stat "$MNT/destination/multiblock" 2>&1
  fail "multiblock data mismatch"
fi
[ "$(cat "$MNT/fallback")" = "root fallback" ] || fail "fallback data mismatch"
[ "$(cat "$MNT/original/conflict")" = "current conflict" ] || fail "collision target changed"
[ "$(stat -c %a "$MNT/original/basic")" = "640" ] || fail "basic mode was not preserved"
umount "$MNT" || fail "final unmount failed"

echo SMOKE_OK
