/**
 * @file mkfs_bsfs.c
 * @brief Formatter utility for the BSFS filesystem image.
 *
 * Formats a regular file (e.g., created with `dd`) into a BSFS filesystem
 * image. The on-disk layout initialized by this tool is:
 *
 * - Block 0: Superblock
 * - Blocks [1 .. 1 + block_bitmap_total - 1]: Linked list of bitmap blocks
 * - Blocks [data_block_start .. total_blocks - 1]: Data and B-tree regions
 *
 * The formatter writes and persists the superblock, initializes the block
 * bitmap linked list, reserves metadata blocks in the bitmap, initializes the
 * inode B-tree (persisting its root in the superblock if created), creates a
 * fresh directory B-tree for the filesystem root, inserts "." and ".." entries,
 * and finally persists the root inode into the inode B-tree.
 *
 * Usage: `mkfs.bsfs &lt;partition_file&gt;`
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <linux/fs.h>

// linux/fs.h defines BLOCK_SIZE as 1024; ensure our filesystem's BLOCK_SIZE
// prevails
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

#include "../include/bitmap.h"
#include "../include/bsfs.h"
#include "../include/btree.h"
#include "../include/inode.h"
#include "../include/mkfs.h"
#include "../include/super.h"

/**
 * @brief Print a formatted error message to stderr.
 *
 * Utility used for concise error reporting within this module.
 *
 * @param msg Null-terminated error message to display.
 */
static void die(const char *msg) {
  fprintf(stderr, "mkfs.bsfs: %s\n", msg);
}

/**
 * @brief Compute the ceiling of a/b for 32-bit unsigned integers.
 *
 * @param a Numerator.
 * @param b Denominator (must be > 0).
 * @return The smallest integer value greater than or equal to a/b.
 */
static uint64_t ceil_div_u64(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

/**
 * @brief Compute the number of bitmap blocks required for the filesystem.
 *
 * Each bitmap block stores BITMAP_BLOCK_BYTES*8 bits plus a 32-bit next pointer
 * (managed by init_bitmap_list). This computes how many bitmap blocks are
 * needed to track all blocks in the filesystem image.
 *
 * @param total_blocks Total number of blocks in the filesystem image.
 * @return Number of bitmap blocks (at least 1).
 */
static uint32_t compute_bitmap_blocks(uint64_t total_blocks) {
  const uint32_t bits_per_block = (uint32_t)(BITMAP_BLOCK_BYTES * 8u);
  uint32_t       blocks         = (uint32_t)ceil_div_u64(total_blocks, bits_per_block);
  if (blocks == 0)
    blocks = 1;
  return blocks;
}

/**
 * @brief Mark metadata blocks (superblock and bitmap region) as allocated.
 *
 * Loads the bitmap, marks blocks [0 .. data_block_start-1] as in-use and
 * persists the updated bitmap back to disk.
 *
 * @param disk Open filesystem image.
 * @param sb   Pointer to an initialized superblock with bitmap positions.
 * @return 0 on success; -1 on failure to load or write the bitmap.
 */
static int reserve_initial_blocks(FILE *disk, const bsfs_super_block *sb) {
  // Mark as in-use: block 0 (superblock) and the whole bitmap region
  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap) {
    fprintf(stderr, "mkfs.bsfs: failed to load block bitmap.\n");
    return -1;
  }

  uint32_t limit = sb->data_block_start; // metadata blocks
  if (limit > sb->total_blocks)
    limit = sb->total_blocks;

  for (uint32_t i = 0; i < limit; i++) {
    bsfs_set_bit(bitmap, i);
  }

  if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
    fprintf(stderr, "mkfs.bsfs: failed to write block bitmap.\n");
    free(bitmap);
    return -1;
  }
  free(bitmap);
  return 0;
}

/**
 * @brief Format an already opened partition file as a BSFS filesystem.
 *
 * This function expects the file size to be a multiple of the block size
 * (4096 bytes). It computes the on-disk layout, writes the superblock,
 * initializes and persists the bitmap, ensures the inode B-tree exists,
 * creates the root directory entries ("." and ".."), and writes the root
 * inode into the inode B-tree. On success, the file is a valid BSFS image.
 *
 * @param disk Opened FILE* corresponding to the partition image (opened in
 *             read/write mode).
 * @return 0 on success; -1 on failure.
 */
static int get_fs_size_bytes(FILE *disk, uint64_t *out_size) {
  if (!disk || !out_size)
    return -1;
  int         fd = fileno(disk);
  struct stat st;
  if (fstat(fd, &st) != 0)
    return -1;
  if (S_ISBLK(st.st_mode)) {
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
      return -1;
    *out_size = bytes;
    return 0;
  }
  if (S_ISREG(st.st_mode)) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0)
      return -1;
    *out_size = (uint64_t)end;
    return 0;
  }
  return -1;
}

/**
 * @brief Format an already opened partition file as a BSFS filesystem.
 *
 * This function expects the file size to be a multiple of the block size
 * (4096 bytes). It computes the on-disk layout, writes the superblock,
 * initializes and persists the bitmap, ensures the inode B-tree exists,
 * creates the root directory entries ("." and ".."), and writes the root
 * inode into the inode B-tree. On success, the file is a valid BSFS image.
 *
 * @param disk Opened FILE* corresponding to the partition image (opened in
 *             read/write mode).
 * @return 0 on success; -1 on failure.
 */
int mkfs_bsfs_format_open(FILE *disk) {
  if (!disk)
    return -1;

  uint64_t fsz = 0;
  if (get_fs_size_bytes(disk, &fsz) != 0) {
    perror("mkfs.bsfs: size");
    return -1;
  }
  if (fsz == 0) {
    die("empty partition file (use dd to create a sized file)");
    return -1;
  }
  if ((fsz % BLOCK_SIZE) != 0) {
    uint64_t aligned = fsz - (fsz % BLOCK_SIZE);
    if (aligned < (uint64_t)BLOCK_SIZE * 4) {
      die("file size is not a multiple of block size (4096)");
      return -1;
    }
    fprintf(stderr, "mkfs.bsfs: warning: size not multiple of %u; truncating to %" PRIu64 " bytes\n", BLOCK_SIZE, aligned);
    fsz = aligned;
  }

  uint64_t fs_size      = fsz;
  uint64_t total_blocks = fs_size / BLOCK_SIZE;
  if (total_blocks < 4) {
    die("minimum size not met (need at least 4 blocks)");
    return -1;
  }

  if (total_blocks > 0xFFFFFFFFULL) {
    die("filesystem too large for 32-bit block addressing (limit ~16 TiB at "
        "4K)");
    return -1;
  }
  uint32_t bitmap_blocks = compute_bitmap_blocks(total_blocks);
  uint32_t data_start    = 1 + bitmap_blocks;
  if (data_start >= total_blocks) {
    die("insufficient space for data region after metadata");
    return -1;
  }

  bsfs_super_block sb;
  memset(&sb, 0, sizeof(sb));
  sb.magic_number       = MAGIC_NUMBER;
  sb.block_size         = BLOCK_SIZE;
  sb.fs_size            = fs_size;
  sb.total_blocks       = (uint32_t)total_blocks;
  sb.block_bitmap_start = 1;
  sb.block_bitmap_total = bitmap_blocks;
  sb.inode_root         = 0;
  sb.recovery_root      = 0;
  sb.data_block_start   = data_start;
  sb.root_inode         = 1;

  if (write_superblock(disk, &sb) != 0) {
    die("failed to write superblock");
    return -1;
  }

  if (init_bitmap_list(disk, sb.block_bitmap_start, sb.block_bitmap_total) != 0) {
    die("failed to initialize bitmap list");
    return -1;
  }

  if (reserve_initial_blocks(disk, &sb) != 0) {
    die("failed to reserve metadata blocks in bitmap");
    return -1;
  }

  btree_handle_t inode_handle;
  if (!inode_tree_init(disk, &sb, &inode_handle)) {
    die("failed to initialize inode B-tree");
    return -1;
  }

  btree_handle_t *dir_handle = btree_create_dir_entry_handle();
  if (!dir_handle) {
    die("failed to create directory B-tree handle");
    return -1;
  }

  dir_entry_t dot;
  memset(&dot, 0, sizeof(dot));
  dot.inode_number = sb.root_inode;
  dot.file_type    = FILE_TYPE_DIR;
  dot.file_size    = 0;
  strncpy(dot.filename, ".", MAX_FILENAME);
  if (btree_insert(disk, &sb, dir_handle, &dot) != 0) {
    free(dir_handle);
    die("failed to insert '.' entry into root directory");
    return -1;
  }

  dir_entry_t dotdot;
  memset(&dotdot, 0, sizeof(dotdot));
  dotdot.inode_number = sb.root_inode;
  dotdot.file_type    = FILE_TYPE_DIR;
  dotdot.file_size    = 0;
  strncpy(dotdot.filename, "..", MAX_FILENAME);
  if (btree_insert(disk, &sb, dir_handle, &dotdot) != 0) {
    free(dir_handle);
    die("failed to insert '..' entry into root directory");
    return -1;
  }

  bsfs_inode root;
  memset(&root, 0, sizeof(root));
  root.inode_number = sb.root_inode;
  root.file_size    = 0;
  root.file_type    = FILE_TYPE_DIR;
  root.block_count  = 0;
  root.uid          = 0;
  root.gid          = 0;
  root.permissions  = 0755;
  root.links_count  = 1;
  root.created_at = root.modified_at = root.accessed_at = (uint32_t)time(NULL);
  root.btree_root                                       = dir_handle->root_block;
  for (int i = 0; i < 16; i++) {
    root.direct_blocks[i].file_blk = 0;
    root.direct_blocks[i].disk_blk = 0;
    root.direct_blocks[i].length   = 0;
  }
  if (insert_inode(disk, &sb, &inode_handle, &root) != 0) {
    free(dir_handle);
    die("failed to insert root inode into inode B-tree");
    return -1;
  }

  (void)write_superblock(disk, &sb);
  free(dir_handle);

  printf("BSFS formatted successfully\n");
  printf("Partition size (bytes): %" PRIu64 "\n", sb.fs_size);
  printf("Block size (bytes): %u\n", sb.block_size);
  printf("Total blocks: %u\n", sb.total_blocks);
  printf("Block bitmap (start,total): %u,%u\n", sb.block_bitmap_start, sb.block_bitmap_total);
  printf("Data block start: %u\n", sb.data_block_start);
  printf("Inode root block: %u\n", sb.inode_root);
  printf("Root inode: %u\n", sb.root_inode);

  return 0;
}

/**
 * @brief Format a partition file located at @p path as a BSFS filesystem.
 *
 * Opens the target file in read/write mode and delegates to
 * mkfs_bsfs_format_open(). The file must already exist and its size must be a
 * multiple of 4096 bytes.
 *
 * @param path Path to the pre-sized partition file.
 * @return 0 on success; -1 on error (open failure or formatting failure).
 */
int mkfs_bsfs_format_path(const char *path) {
  if (!path)
    return -1;
  FILE *disk = fopen(path, "rb+");
  if (!disk) {
    perror("mkfs.bsfs: fopen");
    return -1;
  }
  int rc = mkfs_bsfs_format_open(disk);
  fclose(disk);
  return rc;
}

/**
 * @brief Program entry point for mkfs.bsfs.
 *
 * Usage: mkfs.bsfs &lt;partition_file&gt;
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return EXIT_SUCCESS on success; EXIT_FAILURE on error.
 */
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <partition_file>\n", argv[0]);
    return EXIT_FAILURE;
  }
  int rc = mkfs_bsfs_format_path(argv[1]);
  return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
