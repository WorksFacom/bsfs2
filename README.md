# BSFS

BSFS (Basic Solution File System) is an educational, userspace file system that stores its data in a regular file or directly on a block device. It comes with a formatter (`mkfs.bsfs`) that writes the on-disk layout and an interactive browser with a shell-like interface for file, directory, recovery and permission operations. Deleted files go into a recovery tree and can be restored one by one.

More detailed code documentation is online [here](https://facom.ufms.br/~brivaldo/bsfs/).

## Requirements

- POSIX-compliant environment (tested on Linux)
- `gcc` (or compatible C compiler)
- `make`

## Installation

1. Clone the repository and enter the project directory.
2. Clean any previous build artifacts: `make clean`
3. Build the formatter and browser binaries: `make`

Binaries go in `bin/`. `make` builds `bin/mkfs_bsfs`, an alias `bin/mkfs.bsfs`, and the interactive `bin/browser`.

## Usage

Format a file-backed image (or a block device), then explore it with the BSFS browser.

### Formatting

1. Create an empty image whose size is a multiple of 4096 bytes. Example for an 8 MiB image:
   ```sh
   dd if=/dev/zero of=bsfs.img bs=1M count=8
   ```
2. Format the image with the BSFS formatter:
   ```sh
   ./bin/mkfs.bsfs bsfs.img
   ```
   This writes the superblock, the block bitmap, the inode and directory B-trees, and the root directory entries.
3. You can also format a raw block device, such as a partition like `/dev/sdaX`. Check that it is unmounted and that the path is right first:
   ```sh
   sudo ./bin/mkfs.bsfs /dev/sdaX
   ```
   This does the same thing in place on the device.

### Browser

Start the browser on the formatted image or device:
```sh
./bin/browser bsfs.img
```

For a partition, pass the device path instead of a file:
```sh
sudo ./bin/browser /dev/sdaX
```

The browser accepts the commands below, the same list `help` prints. `rm` moves entries into a recovery B-tree, `recovery` lists them, `restore` brings one back by name or ID, and `purge` drops them for good.
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

Leave with `exit`. For scripted demos, pipe commands through `stdin`:
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

For API docs, run `doxygen Doxyfile` from the project root. HTML goes to `docs/html` and LaTeX to `docs/latex`.
