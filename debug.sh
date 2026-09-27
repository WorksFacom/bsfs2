SCRIPT_DIR="$(dirname "$(realpath "$0")")"
[ -f "$SCRIPT_DIR/local.mk" ] && source "$SCRIPT_DIR/local.mk"

gdb "$KDIR/vmlinux" -ex "target remote :1234"
