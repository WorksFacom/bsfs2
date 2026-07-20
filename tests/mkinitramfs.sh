#!/bin/bash
# Gera o initramfs usado pelas VMs de teste (start_qemu.sh e tests/smoke.sh).
#
# Produz $KDIR/initramfs.cpio.gz a partir de um busybox estatico do host.
# O /init resultante monta /proc, /sys, /dev e o share 9p do host em /mnt/host;
# se a cmdline do kernel contiver "bsfs.smoke", roda tests/smoke_guest.sh e
# desliga a VM, caso contrario cai num shell interativo.
#
# Uso:  ./tests/mkinitramfs.sh
# Requer: busybox (dnf install busybox), cpio, gzip.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$SCRIPT_DIR/.env" ] && source "$SCRIPT_DIR/.env"
: "${KDIR:?KDIR nao definido — copie .env.example para .env}"

BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"
[ -x "$BUSYBOX" ] || { echo "busybox nao encontrado (dnf install busybox)"; exit 1; }
if ! file -L "$BUSYBOX" | grep -q "statically linked"; then
	echo "AVISO: $BUSYBOX nao parece estatico; o initramfs pode nao bootar."
fi

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

mkdir -p "$ROOT"/{bin,sbin,proc,sys,dev,tmp,etc,mnt/host}
cp "$BUSYBOX" "$ROOT/bin/busybox"

# Um symlink por applet, para que sh/mount/insmod/losetup/... existam no PATH.
(cd "$ROOT" && ./bin/busybox --list-full | while read -r applet; do
	mkdir -p "$(dirname "$applet")"
	ln -sf /bin/busybox "$applet"
done)

# O mkfs.bsfs roda no convidado a partir do share 9p, mas e um binario dinamico:
# copia a libc/loader do host para dentro do initramfs para que ele execute.
if [ -x "$SCRIPT_DIR/mkfs.bsfs" ]; then
	ldd "$SCRIPT_DIR/mkfs.bsfs" 2>/dev/null | grep -oE '/[^ ]*\.so[^ ]*' | sort -u | while read -r lib; do
		[ -e "$lib" ] || continue
		mkdir -p "$ROOT$(dirname "$lib")"
		cp -L "$lib" "$ROOT$lib"
	done
else
	echo "AVISO: mkfs.bsfs nao encontrado; rode 'make' antes para embutir as libs dele."
fi

cat > "$ROOT/init" <<'INIT'
#!/bin/sh
mount -t proc     none /proc
mount -t sysfs    none /sys
mount -t devtmpfs none /dev 2>/dev/null

mkdir -p /mnt/host
if ! mount -t 9p -o trans=virtio,version=9p2000.L,msize=512000 hostshare /mnt/host; then
	echo "AVISO: falha ao montar o share 9p (hostshare)"
fi

if grep -q 'bsfs\.smoke' /proc/cmdline; then
	sh /mnt/host/tests/smoke_guest.sh
	echo SMOKE_DONE
	sync
	poweroff -f
fi

echo
echo "=== BSFS test VM === (share do host em /mnt/host)"
echo
exec setsid cttyhack sh
INIT
chmod +x "$ROOT/init"

OUT="$KDIR/initramfs.cpio.gz"
(cd "$ROOT" && find . -print0 | cpio --null -H newc -o 2>/dev/null) | gzip -9 > "$OUT"

echo "initramfs gerado: $OUT ($(du -h "$OUT" | cut -f1))"
