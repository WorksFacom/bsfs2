#!/bin/bash
# builds $KDIR/initramfs.cpio.gz for the test vms (start_qemu.sh, tests/smoke.sh) from the host's static busybox.
# /init mounts /proc, /sys, /dev and the 9p share at /mnt/host. with bsfs.run=<script> on the cmdline it
# runs the script and powers off, otherwise you get a shell.
#
# usage: ./tools/mkinitramfs.sh
# needs busybox, cpio, gzip and file on PATH

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$SCRIPT_DIR/local.mk" ] && source "$SCRIPT_DIR/local.mk"
: "${KDIR:?KDIR not set, copy local.mk.example to local.mk}"

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

# one symlink per applet
(cd "$ROOT" && ./bin/busybox --list-full | while read -r applet; do
  # some builds list the multicall binary itself, don't turn it into a self symlink
  [ "$applet" = "bin/busybox" ] && continue
  mkdir -p "$(dirname "$applet")"
  ln -sf /bin/busybox "$applet"
done)

# mkfs.bsfs runs from the 9p share and is dynamic, so ship the host's libc and loader
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

# bsfs.run=<repo relative path> runs that script from the share, then powers off
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
