/**
 * @file commands_bsfs.c
 * @brief Core browser commands for the BSFS filesystem.
 *
 * Implements basic browser functionality including exit and info commands.
 */
#include "../include/commands_bsfs.h"

/**
 * @brief Exit the BSFS browser loop.
 *
 * Usage: exit
 *
 * Prints a farewell message and returns 0 so the main loop terminates.
 * @param file Open filesystem image (unused).
 * @param sb   Loaded superblock (unused).
 * @param args Tokenized arguments (unused).
 * @return 0 to signal the REPL to exit.
 */
int bsfs_cmd_exit(FILE *file, superblock_t *sb, char **args) {
  (void)file;
  (void)sb;
  (void)args;
  fprintf(stdout, "Leaving BSFS Browser...\n");
  return 0;
}

/**
 * @brief Print filesystem information from the superblock.
 *
 * Usage: info
 *
 * Dumps key fields such as magic, block size, total blocks, bitmap layout,
 * inode and recovery roots, and the root inode number.
 * @param file Open filesystem image (unused).
 * @param sb   Loaded superblock.
 * @param args Tokenized arguments (unused).
 * @return 1 to continue the REPL.
 */
int bsfs_cmd_info(FILE *file, superblock_t *sb, char **args) {
  (void)file;
  (void)args;
  fprintf(stdout, "Magic Number: %u\n", sb->magic_number);
  fprintf(stdout, "Block size (bytes): %u\n", sb->block_size);
  fprintf(stdout, "File System Size (bytes): %llu\n",
          (unsigned long long)sb->fs_size);
  fprintf(stdout, "Total Blocks: %u\n", sb->total_blocks);
  fprintf(stdout, "Block Bitmap Start: %u\n", sb->block_bitmap_start);
  fprintf(stdout, "Block Bitmap Total: %u\n", sb->block_bitmap_total);
  fprintf(stdout, "Data Blocks Start: %u\n", sb->data_block_start);
  fprintf(stdout, "Inode Root Block: %u\n", sb->inode_root);
  fprintf(stdout, "Recovery Root Block: %u\n", sb->recovery_root);
  fprintf(stdout, "Root Inode: %u\n", sb->root_inode);
  return 1;
}

/**
 * @brief Display a help summary for available commands.
 *
 * Usage: help
 *
 * Prints categories and usage examples for navigation, file, directory,
 * recovery and system commands available in the browser.
 * @param file Open filesystem image (unused).
 * @param sb   Loaded superblock (unused).
 * @param args Tokenized arguments (unused).
 * @return 1 to continue the REPL.
 */
int bsfs_cmd_help(FILE *file, superblock_t *sb, char **args) {
  (void)file;
  (void)sb;
  (void)args;

  fprintf(stdout, "BSFS Browser Commands:\n");
  fprintf(stdout, "======================\n");
  fprintf(stdout, "Navigation:\n");
  fprintf(stdout, "  cd <directory>     - Change current directory\n");
  fprintf(stdout, "  whoami             - Show current uid/gid and umask\n");
  fprintf(stdout, "  su <uid> [gid]     - Switch session identity (testing)\n");
  fprintf(stdout, "  umask [octal]      - Show/set umask\n");
  fprintf(stdout, "  pwd                - Print current working directory\n");
  fprintf(stdout, "  ls [directory]     - List directory contents\n");
  fprintf(stdout,
          "  list [directory]   - List directory contents (alias for ls)\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "Directory Operations:\n");
  fprintf(stdout, "  mkdir <directory>  - Create a new directory\n");
  fprintf(stdout, "  rmdir <directory>  - Remove an empty directory\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "File Operations:\n");
  fprintf(stdout, "  touch <filename>   - Create an empty file\n");
  fprintf(stdout,
          "  rm <filename>      - Remove a file (rm --all [dir] sweeps files with timing)\n");
  fprintf(stdout, "  cp <src> <dest>    - Copy a file\n");
  fprintf(stdout, "  mv <src> <dest>    - Move/rename a file or directory\n");
  fprintf(stdout, "  cat <filename>     - Display file contents\n");
  fprintf(stdout,
          "  fill <count> <size> [prefix] [bs=<n>] [pattern=index|zero|random]\n");
  fprintf(stdout,
          "       Create many files and print elapsed time (size/bs accept K/M/G)\n");
  fprintf(stdout,
          "  chmod <mode> <path>- Change permissions (octal, e.g., 755)\n");
  fprintf(stdout, "  chown <uid> <path> - Change file owner (root only)\n");
  fprintf(stdout, "  chgrp <gid> <path> - Change file group (root only)\n");
  fprintf(stdout, "  echo <text>        - Display text or write to file\n");
  fprintf(stdout,
          "    echo \"text\" > file   - Write text to file (overwrite)\n");
  fprintf(stdout, "    echo \"text\" >> file  - Append text to file\n");
  fprintf(stdout, "  dd if=<src> of=<dest> bs=<size> count=<blocks> - Create "
                  "files of specific size\n");
  fprintf(stdout, "    Example: dd if=/dev/zero of=testfile bs=1M count=10 "
                  "(creates 10MB file)\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "System:\n");
  fprintf(stdout, "  info               - Display filesystem information\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "Recovery:\n");
  fprintf(stdout, "  recovery [name]    - List recovery entries (optionally "
                  "filter by name)\n");
  fprintf(stdout, "  restore <id|name> [dir] - Restore a deleted file by "
                  "recovery id or by original name (optional dest dir)\n");
  fprintf(stdout, "  purge [id|oldest N|expired] - Purge recovery entries "
                  "(default: older than ~30 days)\n");
  fprintf(stdout, "  help               - Display this help message\n");
  fprintf(stdout, "  exit               - Exit the BSFS browser\n");

  return 1;
}
