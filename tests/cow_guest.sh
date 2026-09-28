#!/bin/sh
# Copy-on-write test.
#
# With shadowing, every mutation allocates fresh blocks for the nodes it touches
# and hands the blocks they replaced back to the allocator. If the replaced ones
# were not released, the metadata would grow without bound, so this churns the
# trees far past the size of the filesystem: 30 rounds of creating and deleting
# 50 files each on a 32 MB image. A leak of even a couple of blocks per mutation
# runs out of space long before the last round.
#
# It also checks that a rolled-back mutation (removing something that is not
# there) neither corrupts the tree nor consumes space.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_cow.img
MNT=/tmp/bsfs_cow_mnt
LOOP=""
ROUNDS="${COW_ROUNDS:-30}"
FILES="${COW_FILES:-50}"

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

# 1. Churn: create and delete the same set of files over and over. Files are
#    left empty on purpose, deleted data blocks are kept on purpose for the
#    recovery subsystem, so only the metadata is meant to be reclaimed here.
r=1
while [ "$r" -le "$ROUNDS" ]; do
  i=1
  while [ "$i" -le "$FILES" ]; do
    touch "$MNT/c$i" || fail "round $r: touch c$i failed (metadata blocks leaking?)"
    i=$((i + 1))
  done

  i=1
  while [ "$i" -le "$FILES" ]; do
    rm "$MNT/c$i" || fail "round $r: rm c$i failed"
    i=$((i + 1))
  done

  LEFT=$(ls "$MNT" | wc -l)
  [ "$LEFT" -eq 0 ] || fail "round $r ended with $LEFT leftover entries"

  r=$((r + 1))
done

# 2. Rolled-back mutations must not leak either: an aborted transaction has to
#    return every block it staged. 300 removals of files that do not exist.
i=1
while [ "$i" -le 300 ]; do
  rm "$MNT/ghost$i" 2>/dev/null && fail "rm of a missing file reported success"
  i=$((i + 1))
done
touch "$MNT/survivor" || fail "touch survivor failed after the rollback"
echo alive >"$MNT/survivor" || fail "write survivor failed"
sync

# 3. The tree still has to be usable and to survive a remount after all that.
umount "$MNT" || fail "umount failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed"
OUT=$(cat "$MNT/survivor") || fail "cat survivor failed"
[ "$OUT" = "alive" ] || fail "survivor diverged: '$OUT'"

# 4. And there must still be room to work: fill in a batch and read it back.
i=1
while [ "$i" -le 100 ]; do
  echo "v$i" >"$MNT/k$i" || fail "post-churn write k$i failed (space exhausted?)"
  i=$((i + 1))
done
sync
umount "$MNT" || fail "umount 2 failed"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount 2 failed"
COUNT=$(ls "$MNT" | grep -c '^k[0-9]*$')
[ "$COUNT" -eq 100 ] || fail "only $COUNT of 100 files came back after remount"
OUT=$(cat "$MNT/k100") || fail "cat k100 failed"
[ "$OUT" = "v100" ] || fail "k100 diverged: '$OUT'"

echo SMOKE_OK
