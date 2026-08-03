#!/bin/sh
# ENOSPC crash test, phase 1: fill the device, then force the allocator to go
# looking for the blocks the deferred-free list is holding, and die.
#
# This is the case the plain crash test does not reach. The allocator used to
# commit the deferred frees inline when it ran dry, and that "commit" only
# flushed the block device - it could not write back the per-inode extent roots,
# which live in dirty VFS inodes until ->write_inode runs. So blocks belonging
# to a tree that was still the on-disk one got handed straight out and
# overwritten.
#
# The victim below is set up to be exactly that: a checkpointed file whose
# extent tree is then rewritten (making the old nodes deferred) without ever
# being synced, followed by enough pressure to make the allocator want those
# blocks back.

set -u

REPO=/mnt/host
MNT=/tmp/enospc_mnt
DEV=/dev/vda

fail() {
  echo "PHASE1_FAIL: $*"
  exit 1
}

"$REPO/mkfs.bsfs" "$DEV" >/dev/null || fail mkfs
insmod "$REPO/bsfs2.ko" || fail insmod
mkdir -p "$MNT"
mount -t bsfs "$DEV" "$MNT" || fail mount

# Checkpoint: several multi-block files, each with a real extent tree, plus the
# checksums phase 2 will verify. 8 blocks each so the tree has internal nodes.
i=1
while [ "$i" -le 12 ]; do
  dd if=/dev/urandom of="$MNT/victim$i" bs=4096 count=8 2>/dev/null || fail "write victim$i"
  i=$((i + 1))
done
sync

i=1
while [ "$i" -le 12 ]; do
  md5sum "$MNT/victim$i" | awk '{print $1}' >"$MNT/sum$i" || fail "sum victim$i"
  i=$((i + 1))
done
sync
echo "PHASE1: checkpoint with $(ls "$MNT" | grep -c '^victim') victims"

# Rewrite each victim in place. This copy-on-writes its extent tree: the new
# root only exists in the dirty in-memory inode, and every node it replaced is
# now on the deferred-free list. No sync from here on.
i=1
while [ "$i" -le 12 ]; do
  dd if=/dev/zero of="$MNT/victim$i" bs=4096 count=1 seek=4 conv=notrunc 2>/dev/null || fail "dirty victim$i"
  i=$((i + 1))
done

# Now squeeze, all the way to a real refusal. Every one of these allocations is
# a chance for the allocator to decide the deferred list is fair game - and the
# blocks on it belong to extent trees that are still the ones on disk.
i=0
while [ "$i" -lt 8192 ]; do
  dd if=/dev/zero of="$MNT/filler" bs=4096 count=1 seek="$i" conv=notrunc 2>/dev/null || break
  i=$((i + 1))
done

# metadata pressure too: these split the inode and dentry trees
j=1
while [ "$j" -le 400 ]; do
  touch "$MNT/churn$j" 2>/dev/null || break
  j=$((j + 1))
done

free_k=$(df -Pk "$MNT" | awk 'NR == 2 { print $4 }')
echo "PHASE1: pressure applied, filler=$i churn=$((j - 1)) free=${free_k}K"
[ "$i" -lt 8192 ] || fail "the device never filled up, no allocation pressure"

echo "PHASE1: crashing the VM without unmounting"
echo b >/proc/sysrq-trigger
sleep 5
fail "sysrq did not crash the VM"
