# BSFS

BSFS (Basic Solution File System) is an educational, userspace file system that stores its data inside a regular file or directly on a block device. The project ships a formatter (`mkfs.bsfs`) that initializes the on-disk layout and an interactive browser that exposes a shell-like interface for exercising core file, directory, recovery, and permission operations. Its standout capability is a built-in recovery subsystem that retains deleted files and lets you restore them selectively.

A more compreensive documentation about the code is online [here](https://facom.ufms.br/~brivaldo/bsfs/).

## Requirements

- POSIX-compliant environment (tested on Linux)
- `gcc` (or compatible C compiler)
- `make`

## Instalation

1. Clone the repository and enter the project directory.
2. Clean any previous build artifacts: `make clean`
3. Build the formatter and browser binaries: `make`

Build outputs are placed in `bin/`. `make` produces `bin/mkfs_bsfs`, creates a convenience alias `bin/mkfs.bsfs`, and emits the interactive `bin/browser` binary.

## Usage

After compiling the project you can format a file-backed image (or even a block device) and explore it through the BSFS browser. The sections below walk through both steps.

### Formating

1. Create an empty image whose size is a multiple of 4096 bytes. Example for an 8 MiB image:
   ```sh
   dd if=/dev/zero of=bsfs.img bs=1M count=8
   ```
2. Format the image with the BSFS formatter:
   ```sh
   ./bin/mkfs.bsfs bsfs.img
   ```
   The formatter writes the superblock, initializes the block bitmap, seeds inode and directory B-trees, and creates the root directory entries.
3. Alternatively, you may format a raw block device (for example a disk partition such as `/dev/sdaX`). Make sure the target device is unmounted and that you have selected the correct path before running:
   ```sh
   sudo ./bin/mkfs.bsfs /dev/sdaX
   ```
   This performs the same initialization in-place on the device.

### Browser

Launch the interactive browser, pointing it at the formatted image or device:
```sh
./bin/browser bsfs.img
```

To inspect a formatted partition directly, pass the device path instead of a file:
```sh
sudo ./bin/browser /dev/sdaX
```

While inside the browser you can use the commands listed below (this matches the built-in `help` output). The recovery flow (`rm`, `recovery`, `restore`, `purge`) showcases the filesystem's flagship feature: deleted entries are staged into a recovery B-tree and can be restored by name or ID.
```text
BSFS Browser Commands:
======================
Navigation:
  cd <directory>     - Change current directory
  whoami             - Show current uid/gid and umask
  su <uid> [gid]     - Switch session identity (testing)
  umask [octal]      - Show/set umask
  pwd                - Print current working directory
  ls [directory]     - List directory contents
  list [directory]   - List directory contents (alias for ls)

Directory Operations:
  mkdir <directory>  - Create a new directory
  rmdir <directory>  - Remove an empty directory

File Operations:
  touch <filename>   - Create an empty file
  rm <filename>      - Remove a file (rm --all [dir] sweeps files with timing)
  cp <src> <dest>    - Copy a file
  mv <src> <dest>    - Move/rename a file or directory
  cat <filename>     - Display file contents
  fill <count> <size> [prefix] [bs=<n>] [pattern=index|zero|random]
       Create many files and print elapsed time (size/bs accept K/M/G)
  chmod <mode> <path>- Change permissions (octal, e.g., 755)
  chown <uid> <path> - Change file owner (root only)
  chgrp <gid> <path> - Change file group (root only)
  echo <text>        - Display text or write to file
    echo "text" > file   - Write text to file (overwrite)
    echo "text" >> file  - Append text to file
  dd if=<src> of=<dest> bs=<size> count=<blocks> - Create files of specific size
    Example: dd if=/dev/zero of=testfile bs=1M count=10 (creates 10MB file)

System:
  info               - Display filesystem information

Recovery:
  recovery [name]    - List recovery entries (optionally filter by name)
  restore <id|name> [dir] - Restore a deleted file by recovery id or by original name (optional dest dir)
  purge [id|oldest N|expired] - Purge recovery entries (default: older than ~30 days)
  help               - Display this help message
  exit               - Exit the BSFS browser
```

Exit the browser with `exit`. For automated demos you can pipe commands through `stdin`, for example:
```sh
cat <<'CMDS' | ./bin/browser bsfs.img
info
mkdir demo
cd demo
echo "hello BSFS" > a.txt
ls
cat a.txt
rm a.txt
recovery
restore a.txt
ls
exit
CMDS
```

To generate API documentation with Doxygen, run `doxygen Doxyfile` from the project root. The HTML output is written to `docs/html` and LaTeX sources to `docs/latex`.
