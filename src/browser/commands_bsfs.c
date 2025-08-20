/**
 * @file commands_bsfs.c
 * @brief Core browser commands for the BSFS filesystem.
 *
 * Implements basic browser functionality including exit and info commands.
 */
#include "../include/commands_bsfs.h"
#include <stdio.h>

/**
 * @brief Exits the BSFS browser.
 *
 * @param file Unused parameter.
 * @param sb Unused parameter.
 * @param args Unused parameter.
 * @return Always returns 0 to exit the browser loop.
 */
int bsfs_cmd_exit(FILE *file, superblock_t *sb, char **args) {
  (void)file;
  (void)sb;
  (void)args;
  fprintf(stdout, "Leaving BSFS Browser...\n");
  return 0;
}

/**
 * @brief Displays filesystem information from the superblock.
 *
 * @param file Unused parameter.
 * @param sb Pointer to the superblock structure containing filesystem metadata.
 * @param args Unused parameter.
 * @return Always returns 1 to continue the browser loop.
 */
int bsfs_cmd_info(FILE *file, superblock_t *sb, char **args) {
  (void)file;
  (void)args;
  fprintf(stdout, "Magic Number: %d\n", sb->magic_number);
  fprintf(stdout, "Block size(bytes): %d\n", sb->block_size);
  fprintf(stdout, "File System Size (bytes): %d\n", sb->fs_size);
  fprintf(stdout, "Total Inodes: %d\n", sb->total_inodes);
  fprintf(stdout, "Total Blocks: %d\n", sb->total_blocks);
  fprintf(stdout, "Free Inodes: %d\n", sb->free_inodes);
  fprintf(stdout, "Free Blocks: %d\n", sb->free_blocks);
  fprintf(stdout, "Inode Bitmap Start: %d\n", sb->inode_bitmap_start);
  fprintf(stdout, "Inode Bitmap Total: %d\n", sb->inode_bitmap_total);
  fprintf(stdout, "Block Bitmap Start: %d\n", sb->block_bitmap_start);
  fprintf(stdout, "Block Bitmap Total: %d\n", sb->block_bitmap_total);
  fprintf(stdout, "Recovery Bitmap Start: %d\n", sb->rec_bitmap_start);
  fprintf(stdout, "Recovery Bitmap Total: %d\n", sb->rec_bitmap_total);
  fprintf(stdout, "Inode Blocks Start: %d\n", sb->inode_blocks_start);
  fprintf(stdout, "Inode Blocks Total: %d\n", sb->inode_blocks_total);
  fprintf(stdout, "Data Blocks Start: %d\n", sb->data_block_start);
  fprintf(stdout, "Root Inode: %d\n", sb->root_inode);
  return 1;
}

/**
 * @brief Displays help information about available commands.
 *
 * @param file Unused parameter.
 * @param sb Unused parameter.
 * @param args Unused parameter.
 * @return Always returns 1 to continue the browser loop.
 */
int bsfs_cmd_help(FILE *file, superblock_t *sb, char **args) {
  (void)file;
  (void)sb;
  (void)args;
  
  fprintf(stdout, "BSFS Browser Commands:\n");
  fprintf(stdout, "======================\n");
  fprintf(stdout, "Navigation:\n");
  fprintf(stdout, "  cd <directory>     - Change current directory\n");
  fprintf(stdout, "  pwd                - Print current working directory\n");
  fprintf(stdout, "  ls [directory]     - List directory contents\n");
  fprintf(stdout, "  list [directory]   - List directory contents (alias for ls)\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "Directory Operations:\n");
  fprintf(stdout, "  mkdir <directory>  - Create a new directory\n");
  fprintf(stdout, "  rmdir <directory>  - Remove an empty directory\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "File Operations:\n");
  fprintf(stdout, "  touch <filename>   - Create an empty file\n");
  fprintf(stdout, "  rm <filename>      - Remove a file\n");
  fprintf(stdout, "  cp <src> <dest>    - Copy a file\n");
  fprintf(stdout, "  mv <src> <dest>    - Move/rename a file or directory\n");
  fprintf(stdout, "  cat <filename>     - Display file contents\n");
  fprintf(stdout, "  echo <text>        - Display text or write to file\n");
  fprintf(stdout, "    echo \"text\" > file   - Write text to file (overwrite)\n");
  fprintf(stdout, "    echo \"text\" >> file  - Append text to file\n");
  fprintf(stdout, "  dd if=<src> of=<dest> bs=<size> count=<blocks> - Create files of specific size\n");
  fprintf(stdout, "    Example: dd if=/dev/zero of=testfile bs=1M count=10 (creates 10MB file)\n");
  fprintf(stdout, "\n");
  fprintf(stdout, "System:\n");
  fprintf(stdout, "  info               - Display filesystem information\n");
  fprintf(stdout, "  help               - Display this help message\n");
  fprintf(stdout, "  exit               - Exit the BSFS browser\n");
  
  return 1;
}
