#!/bin/sh
# Persistence test: every tree mutation changes a root that has to be written
# back to disk (superblock for the inode/recovery trees, the inode itself for
# the per-inode dentry and bspan trees). This exercises the paths where that
# write-back is easy to miss: multi-block files, truncation, subdirectories and
# emptying a directory until its tree collapses.
#
# Prints "SMOKE_OK" or "SMOKE_FAIL: <reason>"; the runner (tests/smoke.sh) greps
# for that line.

set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMG=/tmp/bsfs_persist.img
MNT=/tmp/bsfs_persist_mnt
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

remount() {
  umount "$MNT" || fail "umount failed ($1)"
  sync
  mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "remount failed ($1)"
}

dd if=/dev/zero of="$IMG" bs=1M count=32 2>/dev/null || fail "dd failed"
MKFS_OUT=$("$REPO/mkfs.bsfs" "$IMG" 2>&1) || fail "mkfs.bsfs failed: $MKFS_OUT"
insmod "$REPO/bsfs2.ko" 2>&1 || fail "insmod failed"
LOOP=$(losetup -f) || fail "losetup -f failed"
losetup "$LOOP" "$IMG" || fail "losetup $LOOP failed"
mkdir -p "$MNT"
mount -t bsfs "$LOOP" "$MNT" 2>&1 || fail "mount failed"

# 1. Multi-block file: spans several bspan entries, so the bspan tree splits and
#    its root must reach the inode on disk.
dd if=/dev/urandom of=/tmp/blob bs=4096 count=64 2>/dev/null || fail "dd blob failed"
cp /tmp/blob "$MNT/big" || fail "cp big failed"
sync
WANT=$(md5sum < /tmp/blob | cut -d' ' -f1)

remount "big"
GOT=$(md5sum < "$MNT/big" | cut -d' ' -f1)
[ "$WANT" = "$GOT" ] || fail "big diverged after remount ($WANT != $GOT)"
SIZE=$(stat -c '%s' "$MNT/big")
[ "$SIZE" -eq 262144 ] || fail "big has size $SIZE after remount, expected 262144"

# 2. Truncation: frees spans and can collapse the bspan tree root back to 0.
: > "$MNT/big" || fail "truncate big failed"
sync
SIZE=$(stat -c '%s' "$MNT/big")
[ "$SIZE" -eq 0 ] || fail "big has size $SIZE right after truncate"

remount "truncate"
SIZE=$(stat -c '%s' "$MNT/big")
[ "$SIZE" -eq 0 ] || fail "big has size $SIZE after remount, expected 0"
OUT=$(cat "$MNT/big") || fail "cat big after truncate failed"
[ -z "$OUT" ] || fail "big is not empty after truncate: '$OUT'"

# 3. Rewriting a truncated file must allocate spans again.
echo "second-life" > "$MNT/big" || fail "rewrite big failed"
sync
remount "rewrite"
OUT=$(cat "$MNT/big") || fail "cat big after rewrite failed"
[ "$OUT" = "second-life" ] || fail "big diverged after rewrite: '$OUT'"

# 4. Subdirectory with its own dentry tree, split included.
mkdir "$MNT/sub" || fail "mkdir sub failed"
i=1
while [ "$i" -le 40 ]; do
  echo "d$i" > "$MNT/sub/d$i" || fail "write sub/d$i failed"
  i=$((i + 1))
done
sync

remount "subdir"
COUNT=$(ls "$MNT/sub" | grep -c '^d[0-9]*$')
[ "$COUNT" -eq 40 ] || fail "sub holds $COUNT of 40 entries after remount"
OUT=$(cat "$MNT/sub/d40") || fail "cat sub/d40 failed"
[ "$OUT" = "d40" ] || fail "sub/d40 diverged: '$OUT'"

# 5. Emptying the directory collapses its dentry tree; the collapsed root (0)
#    must be persisted too, and the directory must stay usable afterwards.
i=1
while [ "$i" -le 40 ]; do
  rm "$MNT/sub/d$i" || fail "rm sub/d$i failed"
  i=$((i + 1))
done
sync

remount "empty dir"
LEFT=$(ls "$MNT/sub" | wc -l)
[ "$LEFT" -eq 0 ] || fail "sub still lists $LEFT entries after removing everything"
echo again > "$MNT/sub/back" || fail "write into the emptied dir failed"
sync
remount "reuse dir"
OUT=$(cat "$MNT/sub/back") || fail "cat sub/back failed"
[ "$OUT" = "again" ] || fail "sub/back diverged: '$OUT'"

# 6. Truncation must actually free the blocks: this writes 40 MB in total into
#    an 8 MB filesystem, one 2 MB file at a time, truncating in between. If
#    ->setattr does not reach bsfs_truncate() the blocks leak and this runs out
#    of space long before the last round.
i=1
while [ "$i" -le 20 ]; do
  dd if=/dev/zero of="$MNT/churn" bs=4096 count=512 2>/dev/null || fail "churn write $i failed (leaked blocks?)"
  sync
  : > "$MNT/churn" || fail "churn truncate $i failed"
  sync
  i=$((i + 1))
done
rm "$MNT/churn" || fail "rm churn failed"

# 7. Regrowing a truncated file must not resurrect the old contents: the spans
#    beyond the new size have to be gone, not just hidden by i_size.
dd if=/dev/urandom of=/tmp/pat bs=4096 count=16 2>/dev/null || fail "dd pat failed"
cp /tmp/pat "$MNT/regrow" || fail "write regrow failed"
sync
: > "$MNT/regrow" || fail "truncate regrow failed"
sync
remount "regrow"
truncate -s 8192 "$MNT/regrow" || fail "extend regrow failed"
sync
BYTES=$(cat "$MNT/regrow" | tr -d '\0' | wc -c)
[ "$BYTES" -eq 0 ] || fail "regrow resurrected $BYTES bytes of stale data"

# 8. statfs and truncating a heavily fragmented file.
#
#    Interleaving writes between two files gives each of them ~150 single-block
#    spans, which is more than one collect pass of bsfs_truncate() holds - it
#    used to silently truncate only part of the file. df(1) also only works now
#    that the filesystem implements ->statfs, so it doubles as the check that
#    every block really came back.
FREE0=$(df -P "$MNT" | awk 'NR==2{print $4}')
[ -n "$FREE0" ] && [ "$FREE0" -gt 0 ] || fail "df reported no free space (statfs missing?)"

i=0
while [ "$i" -lt 150 ]; do
  dd if=/dev/zero of="$MNT/fragA" bs=4096 count=1 seek="$i" conv=notrunc 2>/dev/null || fail "write fragA block $i failed"
  dd if=/dev/zero of="$MNT/fragB" bs=4096 count=1 seek="$i" conv=notrunc 2>/dev/null || fail "write fragB block $i failed"
  i=$((i + 1))
done
sync

FREE1=$(df -P "$MNT" | awk 'NR==2{print $4}')
[ "$FREE1" -lt "$FREE0" ] || fail "df did not shrink after writing 300 blocks ($FREE0 -> $FREE1)"

: > "$MNT/fragA" || fail "truncate fragA failed"
sync
FREE2=$(df -P "$MNT" | awk 'NR==2{print $4}')

# 150 blocks of 4 KiB = 600 KiB; allow slack for the metadata that also moved.
GAINED=$((FREE2 - FREE1))
[ "$GAINED" -ge 500 ] || fail "truncating a fragmented file only returned ${GAINED}K, expected ~600K (sweep stopping early?)"

SIZE=$(stat -c '%s' "$MNT/fragA")
[ "$SIZE" -eq 0 ] || fail "fragA still has size $SIZE"

remount "fragmentado"
SIZE=$(stat -c '%s' "$MNT/fragA")
[ "$SIZE" -eq 0 ] || fail "fragA has size $SIZE after remount"
SIZE=$(stat -c '%s' "$MNT/fragB")
[ "$SIZE" -eq 614400 ] || fail "fragB has size $SIZE after remount, expected 614400"

echo SMOKE_OK
