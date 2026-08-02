#!/bin/bash
# Runner for the two-phase crash test.
#
# Phase 1 builds a checkpoint on a virtio disk and kills the VM with sysrq
# (no unmount, no flush). Phase 2 boots again on the same disk and checks the
# checkpoint survived. This is what exercises the copy-on-write write barrier:
# blocks replaced after the checkpoint must not have been reused before their
# new root was durable.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$SCRIPT_DIR/.env" ] && source "$SCRIPT_DIR/.env"
: "${KDIR:?KDIR not set - copy .env.example to .env}"

IMG=/tmp/bsfs_crash.img
LOG=/tmp/bsfs_crash.log

run_phase() {
  timeout "${CRASH_TIMEOUT:-300}" qemu-system-x86_64 \
    -kernel "$KDIR/arch/x86/boot/bzImage" \
    -initrd "$KDIR/initramfs.cpio.gz" \
    -append "console=ttyS0 nokaslr panic=1 bsfs.run=$1" \
    -m "${SMOKE_MEM:-1024}" \
    -nographic \
    -no-reboot \
    -drive "file=$IMG,format=raw,if=virtio" \
    -fsdev "local,id=fsdev0,path=$SCRIPT_DIR,security_model=none" \
    -device virtio-9p-pci,fsdev=fsdev0,mount_tag=hostshare \
    >>"$LOG" 2>&1
}

dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
: >"$LOG"

run_phase tests/crash_phase1_guest.sh
if ! tr -d '\r' <"$LOG" | grep -q '^PHASE1: derrubando'; then
  echo "INFRA FAIL: phase 1 did not reach the crash. Tail of $LOG:"
  tail -20 "$LOG"
  exit 2
fi

run_phase tests/crash_phase2_guest.sh

if tr -d '\r' <"$LOG" | grep -q '^SMOKE_OK$'; then
  echo "PASS (log: $LOG)"
  exit 0
fi

if tr -d '\r' <"$LOG" | grep -q '^SMOKE_FAIL:'; then
  echo "FAIL:"
  tr -d '\r' <"$LOG" | grep '^SMOKE_FAIL:'
  echo "(full log: $LOG)"
  exit 1
fi

echo "INFRA FAIL. Tail of $LOG:"
tail -20 "$LOG"
exit 2
