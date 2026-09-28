#!/bin/sh
# Reclaim test: the space a deleted file used has to become usable again.
#
# Deleting does not free anything directly, and that is on purpose: the file's
# extent tree is handed to a recovery snapshot so it can be restored. What was
# missing is the other half of the deal: nothing ever purged those snapshots,
# and btree_deallocate_all() had no caller in the module at all, so in practice
# every byte ever deleted stayed gone until the next mkfs. df is expected not to
# move on rm; what must hold is that the blocks come back the moment the
# filesystem actually needs them.
#
# It also covers the corner copy-on-write creates: shadowing allocates before it
# releases, so a filesystem with nothing free still has to be able to delete its
# way out.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_reclaim.img
MNT=/tmp/bsfs_reclaim_mnt
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

free_k() {
  df -Pk "$MNT" | awk 'NR == 2 { print $4 }'
}

# writes single blocks at increasing offsets until the filesystem refuses,
# echoing how many landed
fill() {
  _n=0
  while [ "$_n" -lt "$2" ]; do
    dd if=/dev/zero of="$1" bs=4096 count=1 seek="$_n" conv=notrunc 2>/dev/null || break
    _n=$((_n + 1))
  done
  echo "$_n"
}

dd if=/dev/zero of="$IMG" bs=1M count=32 2>/dev/null || fail "dd failed"
MKFS_OUT=$("$REPO/mkfs.bsfs" "$IMG" 2>&1) || fail "mkfs.bsfs failed: $MKFS_OUT"
insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"
LOOP=$(losetup -f) || fail "losetup -f failed"
losetup "$LOOP" "$IMG" || fail "losetup $LOOP failed"
mkdir -p "$MNT"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "mount failed"

BASE=$(free_k)
[ -n "$BASE" ] && [ "$BASE" -gt 0 ] || fail "df reported no free space"
echo "reclaim: starting with ${BASE}K free"

# 1. Fill the device completely. This must stop cleanly, not oops or succeed
#    forever, and df must agree that there is nothing left.
WROTE=$(fill "$MNT/hog" 8192)
sync
[ "$WROTE" -gt 100 ] || fail "only $WROTE blocks could be written to an empty filesystem"

LEFT=$(free_k)
[ "$LEFT" -lt 256 ] || fail "df still reports ${LEFT}K free after filling the device"
echo "reclaim: filled with $WROTE blocks, ${LEFT}K free"

# 2. Deleting on a full filesystem has to work. Under copy-on-write the removal
#    itself allocates, so without the metadata reserve this wedges forever.
rm "$MNT/hog" || fail "rm on a full filesystem failed (no reserve for the shrink?)"
sync

# 3. The blocks are now held by a recovery snapshot, so df is not expected to
#    jump. What must be true is that writing them again works: the reclaim path
#    purges the snapshot rather than returning ENOSPC.
AGAIN=$(fill "$MNT/second" 8192)
sync
[ "$AGAIN" -ge "$((WROTE / 2))" ] || fail "after deleting $WROTE blocks only $AGAIN could be written back"
echo "reclaim: rewrote $AGAIN blocks of the $WROTE that were deleted"

# 4. And once more, to prove it is repeatable and not a one-shot windfall.
rm "$MNT/second" || fail "rm second failed"
sync
THIRD=$(fill "$MNT/third" 8192)
sync
[ "$THIRD" -ge "$((WROTE / 2))" ] || fail "on the second cycle only $THIRD blocks could be written"

rm "$MNT/third" || fail "rm third failed"
sync

# 5. The filesystem still has to be correct after all that recycling.
i=1
while [ "$i" -le 50 ]; do
  echo "again-$i" >"$MNT/after$i" || fail "write after$i failed once space came back"
  i=$((i + 1))
done
sync

dd if=/dev/urandom of=/tmp/reclaim_blob bs=4096 count=64 2>/dev/null || fail "dd blob failed"
cp /tmp/reclaim_blob "$MNT/blob" || fail "write blob failed"
sync
WANT=$(md5sum </tmp/reclaim_blob | cut -d' ' -f1)

umount "$MNT" || fail "umount failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed"

GOT=$(md5sum <"$MNT/blob" | cut -d' ' -f1)
[ "$WANT" = "$GOT" ] || fail "blob diverged after remount (recycled blocks handed out twice?)"

COUNT=$(ls "$MNT" | grep -c '^after[0-9]*$')
[ "$COUNT" -eq 50 ] || fail "only $COUNT of 50 files came back after remount"

OUT=$(cat "$MNT/after50") || fail "cat after50 failed"
[ "$OUT" = "again-50" ] || fail "after50 diverged: '$OUT'"

echo SMOKE_OK
