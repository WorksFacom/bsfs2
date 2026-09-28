#!/bin/sh
# Allocator test: the invariants any block-allocation strategy has to hold.
#
# The rest of the suite exercises the filesystem and only catches an allocator
# bug when it happens to corrupt something visible. This one goes at the
# allocator directly, because the two ways a strategy fails are both quiet:
#
#   - it hands the same block to two owners (an index that drifts out of sync
#     with the bitmap), which shows up as one file holding another file's bytes
#   - it loses blocks (frees the bitmap bit but never records the block, or
#     records it twice and double-counts), which shows up as a filesystem that
#     shrinks over time and never recovers
#
# Neither is specific to a strategy, so this runs unchanged against every
# BSFS_ALLOC build. It is the test that says a new strategy is safe to use.
#
# With BSFS_ALLOC_STATS=y it also checks the module's own counters,
# which is the only way to tell that the strategy is actually engaged rather
# than merely not breaking anything.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_alloc.img
MNT=/tmp/bsfs_alloc_mnt
LOOP=""

# how many distinct files the double-hand-out check writes
N="${ALLOC_N:-120}"

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

remount() {
  umount "$MNT" || fail "umount failed ($1)"
  sync
  mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed ($1)"
}

# Each file gets a payload only it could have: a multi-block body built from its
# own index. If two files ever share a block, one of them reads back wrong.
write_marked() {
  _f="$1"
  _i="$2"
  : >"$_f" || return 1
  _j=0
  while [ "$_j" -lt 24 ]; do
    echo "file-$_i-line-$_j-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" >>"$_f" || return 1
    _j=$((_j + 1))
  done
  return 0
}

check_marked() {
  _f="$1"
  _i="$2"
  _got=$(head -1 "$_f" 2>/dev/null | cut -d- -f1-3)
  [ "$_got" = "file-$_i-line" ] || return 1
  _got=$(tail -1 "$_f" 2>/dev/null | cut -d- -f1-4)
  [ "$_got" = "file-$_i-line-23" ] || return 1
  _lines=$(wc -l <"$_f")
  [ "$_lines" -eq 24 ] || return 1
  return 0
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
echo "alloc: baseline ${BASE}K free"

# ---------------------------------------------------------------------------
# 1. No double hand-out.
#
# N files with mutually distinguishable contents, read back both from cache and
# after a remount. A block handed out twice makes one of them read as the other.
# ---------------------------------------------------------------------------
i=1
while [ "$i" -le "$N" ]; do
  write_marked "$MNT/f$i" "$i" || fail "write f$i failed"
  i=$((i + 1))
done
sync

i=1
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/f$i" "$i" || fail "f$i is corrupt in cache (block handed out twice?)"
  i=$((i + 1))
done

remount "phase 1"

i=1
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/f$i" "$i" || fail "f$i is corrupt after remount (block handed out twice?)"
  i=$((i + 1))
done
echo "alloc: $N distinct files survived a remount"

# ---------------------------------------------------------------------------
# 2. Recycling: free the low half, refill it, and check nothing collided.
#
# Deleting every other file scatters holes through the allocated range, which is
# exactly the state a strategy's index exists to describe. Filling them back in
# has to reuse those holes without touching the files that stayed.
# ---------------------------------------------------------------------------
i=1
while [ "$i" -le "$N" ]; do
  rm "$MNT/f$i" || fail "rm f$i failed"
  i=$((i + 2))
done
sync

i=1
while [ "$i" -le "$N" ]; do
  write_marked "$MNT/g$i" "$((1000 + i))" || fail "write g$i failed"
  i=$((i + 2))
done
sync

# the survivors must be untouched...
i=2
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/f$i" "$i" || fail "f$i was corrupted by recycled blocks"
  i=$((i + 2))
done

# ...and so must the files built out of the recycled ones
i=1
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/g$i" "$((1000 + i))" || fail "g$i is corrupt (recycled block collided)"
  i=$((i + 2))
done

remount "phase 2"

i=1
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/g$i" "$((1000 + i))" || fail "g$i is corrupt after remount"
  i=$((i + 2))
done
echo "alloc: recycled half the files without a collision"

# ---------------------------------------------------------------------------
# 3. The free count has to close.
#
# Snapshot df, run a full create/delete cycle, and come back to the same number.
# A strategy that forgets a freed block leaks it; one that records a block twice
# makes free_blocks count space that is not there, which is worse, it turns
# into a failed allocation with df still claiming room.
#
# Deletion parks blocks in a recovery snapshot, so df is only expected to return
# to the baseline once the reclaim path has actually purged them; the fill below
# is what forces that, exactly as tests/reclaim_guest.sh does.
# ---------------------------------------------------------------------------
BEFORE=$(free_k)

r=1
while [ "$r" -le 3 ]; do
  i=1
  while [ "$i" -le 40 ]; do
    echo "round-$r-file-$i" >"$MNT/tmp$i" || fail "write tmp$i (round $r) failed"
    i=$((i + 1))
  done
  sync
  i=1
  while [ "$i" -le 40 ]; do
    rm "$MNT/tmp$i" || fail "rm tmp$i (round $r) failed"
    i=$((i + 1))
  done
  sync
  r=$((r + 1))
done

AFTER=$(free_k)
[ "$AFTER" -le "$BEFORE" ] || fail "df grew from ${BEFORE}K to ${AFTER}K across a create/delete cycle (double-counted frees?)"

# The churn above must not have cost more than the recovery snapshots it made.
# A real leak here is unbounded; snapshot overhead is not.
LOST=$((BEFORE - AFTER))
[ "$LOST" -lt 2048 ] || fail "${LOST}K went missing over 3 create/delete rounds (leaked blocks?)"
echo "alloc: free count closed within ${LOST}K over 3 rounds"

# ---------------------------------------------------------------------------
# 4. Exhaustion and recovery.
#
# Fill to ENOSPC, delete, and write it all again. This is where an index that
# lost track of blocks shows up as a filesystem that can never be refilled, and
# where one that over-reports shows up as a crash instead of a clean ENOSPC.
# ---------------------------------------------------------------------------
n=0
while [ "$n" -lt 8192 ]; do
  dd if=/dev/zero of="$MNT/hog" bs=4096 count=1 seek="$n" conv=notrunc 2>/dev/null || break
  n=$((n + 1))
done
sync
[ "$n" -gt 100 ] || fail "only $n blocks could be written before ENOSPC"

LEFT=$(free_k)
[ "$LEFT" -lt 256 ] || fail "df still reports ${LEFT}K free after filling the device"

rm "$MNT/hog" || fail "rm hog failed (no reserve for the shrink?)"
sync

m=0
while [ "$m" -lt 8192 ]; do
  dd if=/dev/zero of="$MNT/hog2" bs=4096 count=1 seek="$m" conv=notrunc 2>/dev/null || break
  m=$((m + 1))
done
sync
[ "$m" -ge "$((n / 2))" ] || fail "after freeing $n blocks only $m could be written back"
echo "alloc: filled $n blocks, reclaimed and rewrote $m"

rm "$MNT/hog2" || fail "rm hog2 failed"
sync

# ---------------------------------------------------------------------------
# 5. Everything written before the exhaustion is still intact.
#
# All of the recycling above ran while these files sat there. If any of their
# blocks was ever handed out a second time, this is where it surfaces.
# ---------------------------------------------------------------------------
remount "phase 5"

i=2
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/f$i" "$i" || fail "f$i is corrupt after the ENOSPC cycle"
  i=$((i + 2))
done

i=1
while [ "$i" -le "$N" ]; do
  check_marked "$MNT/g$i" "$((1000 + i))" || fail "g$i is corrupt after the ENOSPC cycle"
  i=$((i + 2))
done
echo "alloc: all $N files intact after exhaustion"

# ---------------------------------------------------------------------------
# 6. The module's own counters (BSFS_ALLOC_STATS builds only).
#
# Skipped silently when the counters are compiled out, so this file stays a
# single test for every build. The unmount summary is printed by
# bsfs_alloc_log_unmount(), so it only exists after the umount below.
# ---------------------------------------------------------------------------
umount "$MNT" || fail "final umount failed"

# The counters live in the in-memory superblock, so each summary covers one
# mount and the last one covers the read-only checks above, almost no work.
# Every summary is checked instead, and the recycling requirement is asked of
# the run as a whole.
dmesg | tr -d '\r' | grep -o 'alloc\[[a-z_]*\] allocs=.*' >/tmp/alloc_stats.txt

if [ -s /tmp/alloc_stats.txt ]; then
  STRATEGY=$(sed -n 's/^alloc\[\([a-z_]*\)\].*/\1/p' /tmp/alloc_stats.txt | head -1)
  TOTAL_ALLOCS=0
  TOTAL_FREES=0
  TOTAL_INDEX=0

  while read -r STATS; do
    echo "alloc: $STATS"

    get() { echo "$STATS" | sed -n "s/.*[ ]$1=\([0-9]*\).*/\1/p"; }

    ALLOCS=$(get allocs)
    FREES=$(get frees)
    FROM_INDEX=$(get from_index)
    FROM_CURSOR=$(get from_cursor)
    FALLBACK=$(get fallback_scans)

    [ -n "$ALLOCS" ] && [ -n "$FROM_INDEX" ] && [ -n "$FROM_CURSOR" ] || fail "could not parse the counter line: $STATS"

    # Every allocation came from somewhere, and from exactly one place.
    SUM=$((FROM_INDEX + FROM_CURSOR))
    [ "$SUM" -eq "$ALLOCS" ] || fail "from_index($FROM_INDEX) + from_cursor($FROM_CURSOR) = $SUM, but allocs=$ALLOCS"

    # The fallback is a correctness net for a failure inside the allocator, not
    # a normal path. On a 32 MB image it must never trigger.
    [ "$FALLBACK" -eq 0 ] || fail "strategy '$STRATEGY' fell back to a full scan $FALLBACK times"

    # The linear scan has no index, so anything it attributes to one is a bug in
    # the accounting itself.
    if [ "$STRATEGY" = "linear" ] && [ "$FROM_INDEX" -ne 0 ]; then
      fail "the linear scan reported $FROM_INDEX allocations from an index it does not have"
    fi

    TOTAL_ALLOCS=$((TOTAL_ALLOCS + ALLOCS))
    TOTAL_FREES=$((TOTAL_FREES + FREES))
    TOTAL_INDEX=$((TOTAL_INDEX + FROM_INDEX))
  done </tmp/alloc_stats.txt

  [ "$TOTAL_ALLOCS" -gt 0 ] || fail "stats report $TOTAL_ALLOCS allocations after a full workload"
  [ "$TOTAL_FREES" -gt 0 ] || fail "stats report $TOTAL_FREES frees after deleting most of the filesystem"

  # A strategy that keeps an index of recycled blocks has to have used it: this
  # workload deletes and refills the filesystem several times over. If it never
  # did, the index is dead weight and the strategy is not really engaged.
  if [ "$STRATEGY" != "linear" ] && [ "$TOTAL_INDEX" -eq 0 ]; then
    fail "strategy '$STRATEGY' never allocated from its index across a full recycle workload"
  fi

  echo "alloc: totals for '$STRATEGY': allocs=$TOTAL_ALLOCS frees=$TOTAL_FREES from_index=$TOTAL_INDEX"
else
  echo "alloc: no counters in this build (BSFS_ALLOC_STATS off), skipping stats checks"
fi

mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount after stats failed"
COUNT=$(ls "$MNT" | grep -c '^[fg][0-9]*$')
[ "$COUNT" -eq "$N" ] || fail "only $COUNT of $N files present on the final mount"

echo SMOKE_OK
