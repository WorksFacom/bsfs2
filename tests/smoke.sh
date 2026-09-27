#!/bin/bash
# Host runner for the in-VM tests.
#
#   ./tests/smoke.sh                          # runs tests/smoke_guest.sh
#   ./tests/smoke.sh tests/stress_guest.sh    # runs another script in the guest
#
# Boots QEMU non-interactively, mounts the 9p host share, runs the guest script
# and greps the console output for the SMOKE_OK / SMOKE_FAIL line it prints.
#
# The initramfs (tools/mkinitramfs.sh) drops to a shell as PID 1 normally; with
# "bsfs.run=<script>" on the kernel cmdline its /init runs that script from the
# share instead and powers the VM off. Note: passing a quoted `rdinit=/bin/sh -c "..."`
# does NOT work — the kernel splits the cmdline on whitespace and sh dies with
# "-c: option requires an argument", so the flag lives in the initramfs instead.
#
# Exits 0 on SMOKE_OK, nonzero otherwise. Full console output is kept in
# /tmp/bsfs_smoke.log for debugging.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$SCRIPT_DIR/local.mk" ] && source "$SCRIPT_DIR/local.mk"

: "${KDIR:?KDIR not set — copy local.mk.example to local.mk}"

GUEST="${1:-tests/smoke_guest.sh}"
[ -f "$SCRIPT_DIR/$GUEST" ] || {
  echo "not found: $SCRIPT_DIR/$GUEST"
  exit 2
}

LOG=/tmp/bsfs_smoke.log
TIMEOUT="${SMOKE_TIMEOUT:-90}"

for f in "$KDIR/arch/x86/boot/bzImage" "$KDIR/initramfs.cpio.gz"; do
  [ -f "$f" ] || {
    echo "missing: $f (run ./tools/mkinitramfs.sh and/or build the kernel in \$KDIR)"
    exit 2
  }
done

# Run QEMU under a timeout, capturing all serial output.
timeout "$TIMEOUT" qemu-system-x86_64 \
  -kernel "$KDIR/arch/x86/boot/bzImage" \
  -initrd "$KDIR/initramfs.cpio.gz" \
  -append "console=ttyS0 nokaslr panic=1 bsfs.run=$GUEST" \
  -m "${SMOKE_MEM:-1024}" \
  -nographic \
  -no-reboot \
  -fsdev "local,id=fsdev0,path=$SCRIPT_DIR,security_model=none" \
  -device virtio-9p-pci,fsdev=fsdev0,mount_tag=hostshare \
  >"$LOG" 2>&1

RC=$?

# The serial console sometimes ends lines with CRLF, so normalise first.
if tr -d '\r' <"$LOG" | grep -q '^SMOKE_OK$'; then
  echo "PASS (log: $LOG)"
  exit 0
fi

if grep -q '^SMOKE_FAIL:' "$LOG"; then
  echo "FAIL:"
  tr -d '\r' <"$LOG" | grep '^SMOKE_FAIL:'
  echo "(full log: $LOG)"
  exit 1
fi

# Neither sentinel present — likely infrastructure failure (timeout, boot hang,
# rdinit not honored by this initramfs, etc.). Print the tail so the user can
# see what happened without opening the log.
echo "INFRA FAIL (qemu exit=$RC). Tail of $LOG:"
tail -40 "$LOG"
exit 2
