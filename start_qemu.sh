#!/bin/bash

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
[ -f "$SCRIPT_DIR/.env" ] && source "$SCRIPT_DIR/.env"

qemu-system-x86_64 \
  -kernel "$KDIR/arch/x86/boot/bzImage" \
  -initrd "$KDIR/initramfs.cpio.gz" \
  -append "console=ttyS0 nokaslr" \
  -nographic \
  -fsdev local,id=fsdev0,path=$(pwd),security_model=none \
  -device virtio-9p-pci,fsdev=fsdev0,mount_tag=hostshare
