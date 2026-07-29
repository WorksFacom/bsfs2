#!/bin/sh
# Crash test, phase 1: build a checkpoint, then churn past it and die.
#
# The filesystem lives on a virtio disk (/dev/vda) so it survives the VM being
# killed. After the sync, everything committed is on disk. The churn that
# follows is deliberately *not* synced: it makes copy-on-write replace the very
# nodes the persisted tree still points at. Without the deferred-free barrier
# those blocks would be reused and overwritten right away, so the crash would
# leave the on-disk tree pointing at somebody else's data.
#
# The VM is killed with sysrq (no unmount, no flush). Phase 2 checks the
# checkpoint survived.

set -u
REPO=/mnt/host
MNT=/tmp/crash_mnt
DEV=/dev/vda

"$REPO/mkfs.bsfs" "$DEV" >/dev/null || { echo "PHASE1_FAIL: mkfs"; exit 1; }
insmod "$REPO/bsfs2.ko" || { echo "PHASE1_FAIL: insmod"; exit 1; }
mkdir -p "$MNT"
mount -t bsfs "$DEV" "$MNT" || { echo "PHASE1_FAIL: mount"; exit 1; }

# Checkpoint: 60 files that must still be there after the crash.
i=1
while [ "$i" -le 60 ]; do
  echo "keep-$i" > "$MNT/keep$i" || { echo "PHASE1_FAIL: write keep$i"; exit 1; }
  i=$((i + 1))
done
sync
echo "PHASE1: checkpoint com $(ls "$MNT" | wc -l) arquivos"

# Churn with no sync: forces copy-on-write to replace nodes of the tree that is
# already on disk.
i=1
while [ "$i" -le 60 ]; do
  echo "churn-$i" > "$MNT/churn$i"
  i=$((i + 1))
done
i=1
while [ "$i" -le 60 ]; do
  rm -f "$MNT/churn$i"
  i=$((i + 1))
done

echo "PHASE1: derrubando a VM sem umount"
echo b > /proc/sysrq-trigger
sleep 5
echo "PHASE1_FAIL: sysrq nao derrubou a VM"
