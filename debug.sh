SCRIPT_DIR="$(dirname "$(realpath "$0")")"
[ -f "$SCRIPT_DIR/.env" ] && source "$SCRIPT_DIR/.env"

gdb "$KDIR/vmlinux" -ex "target remote :1234"
