#!/bin/sh
# ENOSPC crash test, phase 2: the checkpoint has to be byte-for-byte intact.
#
# Phase 1 stored each victim's md5 next to it, both synced before any of the
# unsynced churn started. Whatever the crash left behind, the on-disk victims
# must still match those sums: if the allocator handed out blocks belonging to
# the extent tree that was still the durable one, the file reads back as
# somebody else's data and the sum diverges.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner greps for that line.

set -u

REPO=/mnt/host
MNT=/tmp/enospc_mnt
DEV=/dev/vda

fail() {
  echo "SMOKE_FAIL: $*"
  exit 1
}

insmod "$REPO/bsfs2.ko" || fail "insmod failed"
mkdir -p "$MNT"
mount -t bsfs "$DEV" "$MNT" 2>&1 || fail "mount after crash failed (tree corrupted?)"

COUNT=$(ls "$MNT" | grep -c '^victim[0-9]*$')
[ "$COUNT" -eq 12 ] || fail "only $COUNT of 12 victims survived"

i=1
while [ "$i" -le 12 ]; do
  WANT=$(cat "$MNT/sum$i") || fail "sum$i is unreadable"
  GOT=$(md5sum "$MNT/victim$i" | awk '{print $1}') || fail "victim$i is unreadable"

  # phase 1 rewrote block 4 of every victim without syncing, so either the old
  # or the new contents are acceptable. What is not acceptable is a block that
  # belongs to somebody else
  if [ "$WANT" != "$GOT" ]; then
    ZEROED=$(dd if="$MNT/victim$i" bs=4096 skip=4 count=1 2>/dev/null | tr -d '\0' | wc -c)
    [ "$ZEROED" -eq 0 ] || fail "victim$i diverged and block 4 is not the rewrite either (reused metadata?)"
  fi

  SIZE=$(stat -c '%s' "$MNT/victim$i")
  [ "$SIZE" -eq 32768 ] || fail "victim$i has size $SIZE, expected 32768"

  i=$((i + 1))
done

# And the filesystem has to still work afterwards: the allocator must not be
# handing out blocks the crash left marked in use, nor refuse to allocate at all.
rm -f "$MNT/filler"
sync

i=1
while [ "$i" -le 40 ]; do
  echo "post-$i" >"$MNT/post$i" || fail "write post$i after crash failed"
  i=$((i + 1))
done
sync

umount "$MNT" || fail "umount failed"
mount -t bsfs "$DEV" "$MNT" 2>&1 || fail "remount failed"

OUT=$(cat "$MNT/post40") || fail "cat post40 failed"
[ "$OUT" = "post-40" ] || fail "post40 diverged: '$OUT'"

COUNT=$(ls "$MNT" | grep -c '^victim[0-9]*$')
[ "$COUNT" -eq 12 ] || fail "after remount only $COUNT of 12 victims are left"

echo SMOKE_OK
