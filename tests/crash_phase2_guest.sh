#!/bin/sh
# Crash test, phase 2: mount the disk phase 1 left behind and check the
# checkpoint survived intact and the filesystem is still usable.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner greps for that line.

set -u
REPO=/mnt/host
MNT=/tmp/crash_mnt
DEV=/dev/vda

fail() {
  echo "SMOKE_FAIL: $*"
  exit 1
}

insmod "$REPO/bsfs2.ko" || fail "insmod failed"
mkdir -p "$MNT"
mount -t bsfs "$DEV" "$MNT" 2>&1 || fail "mount after crash failed (tree corrupted?)"

# Every checkpointed file must be listed, readable and correct.
COUNT=$(ls "$MNT" | grep -c '^keep[0-9]*$')
[ "$COUNT" -eq 60 ] || fail "only $COUNT of 60 checkpointed files survived"

for i in 1 30 60; do
  OUT=$(cat "$MNT/keep$i") || fail "cat keep$i failed"
  [ "$OUT" = "keep-$i" ] || fail "keep$i diverged: '$OUT'"
done

# And the filesystem has to keep working: the allocator and the bitmap must not
# have been left pointing at blocks that the crash orphaned.
i=1
while [ "$i" -le 30 ]; do
  echo "post-$i" >"$MNT/post$i" || fail "write post$i after crash failed"
  i=$((i + 1))
done
sync
umount "$MNT" || fail "umount failed"
mount -t bsfs "$DEV" "$MNT" 2>&1 || fail "remount failed"
OUT=$(cat "$MNT/post30") || fail "cat post30 failed"
[ "$OUT" = "post-30" ] || fail "post30 diverged: '$OUT'"
COUNT=$(ls "$MNT" | grep -c '^keep[0-9]*$')
[ "$COUNT" -eq 60 ] || fail "after remount only $COUNT of 60 checkpointed files are left"

echo SMOKE_OK
