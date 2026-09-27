#!/bin/bash
# Builds the initramfs used by the test VMs (start_qemu.sh and tests/smoke.sh).
#
# Produces $KDIR/initramfs.cpio.gz out of a static busybox from the host.
# The resulting /init mounts /proc, /sys, /dev and the host's 9p share at
# /mnt/host; with "bsfs.run=<script>" on the kernel cmdline it runs that script
# and powers the VM off, otherwise it drops to an interactive shell.
#
# Usage:   ./tools/mkinitramfs.sh
# Requires: busybox, cpio, gzip, and file on PATH.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$SCRIPT_DIR/local.mk" ] && source "$SCRIPT_DIR/local.mk"
: "${KDIR:?KDIR not set - copy local.mk.example to local.mk}"

BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"
[ -x "$BUSYBOX" ] || {
  echo "busybox not found; install BusyBox and ensure it is on PATH (or set BUSYBOX)"
  exit 1
}
for cmd in cpio gzip file; do
  command -v "$cmd" >/dev/null || {
    echo "$cmd not found; install it and ensure it is on PATH"
    exit 1
  }
done
if ! file -L "$BUSYBOX" | grep -q "statically linked"; then
  echo "WARNING: $BUSYBOX does not look static; the initramfs may not boot."
fi

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

mkdir -p "$ROOT"/{bin,sbin,proc,sys,dev,tmp,etc,mnt/host}
cp "$BUSYBOX" "$ROOT/bin/busybox"

# One symlink per applet, so sh/mount/insmod/losetup/... exist in PATH.
(cd "$ROOT" && ./bin/busybox --list-full | while read -r applet; do
  # Some BusyBox builds list their multicall binary as an applet.  Do not
  # replace the executable we just copied with a symlink to itself.
  [ "$applet" = "bin/busybox" ] && continue
  mkdir -p "$(dirname "$applet")"
  ln -sf /bin/busybox "$applet"
done)

# mkfs.bsfs runs in the guest straight from the 9p share, but it is a dynamic
# binary: copy the host's libc/loader into the initramfs so it can execute.
if [ -x "$SCRIPT_DIR/mkfs.bsfs" ]; then
  ldd "$SCRIPT_DIR/mkfs.bsfs" 2>/dev/null | grep -oE '/[^ ]*\.so[^ ]*' | sort -u | while read -r lib; do
    [ -e "$lib" ] || continue
    mkdir -p "$ROOT$(dirname "$lib")"
    cp -L "$lib" "$ROOT$lib"
  done
else
  echo "WARNING: mkfs.bsfs not found; run 'make' first so its libs get embedded."
fi

cat >"$ROOT/init" <<'INIT'
#!/bin/sh
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev 2>/dev/null

mkdir -p /mnt/host
if ! mount -t 9p -o trans=virtio,version=9p2000.L,msize=512000 hostshare /mnt/host; then
	echo "WARNING: failed to mount the 9p share (hostshare)"
fi

# bsfs.run=<path relative to the repo>: run that script from the share, then
# power the VM off.
RUN=$(sed -n 's/.*bsfs\.run=\([^ ]*\).*/\1/p' /proc/cmdline)
if [ -n "$RUN" ]; then
	sh "/mnt/host/$RUN"
	echo SMOKE_DONE
	sync
	poweroff -f
fi

echo
echo "=== BSFS test VM === (host share at /mnt/host)"
echo
exec setsid cttyhack sh
INIT
chmod +x "$ROOT/init"

OUT="$KDIR/initramfs.cpio.gz"
(cd "$ROOT" && find . -print0 | cpio --null -H newc -o 2>/dev/null) | gzip -9 >"$OUT"

echo "initramfs built: $OUT ($(du -h "$OUT" | cut -f1))"
