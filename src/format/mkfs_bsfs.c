/**
 * @file mkfs_bsfs.c
 * @brief Formatter utility for the BSFS filesystem image.
 *
 * This program formats a file (e.g., created with `dd`) as a BSFS filesystem
 * image. The on-disk layout initialized by this tool is:
 *
 * - Block 0: Superblock
 * - Blocks [1 .. 1 + block_bitmap_total - 1]: Linked list of bitmap blocks
 * - Blocks [data_block_start .. total_blocks - 1]: Data and B-tree regions
 *
 * The formatter writes and persists the superblock, initializes the block
 * bitmap linked list, reserves metadata blocks in the bitmap, initializes the
 * inode B-tree (persisting its root in the superblock if created), creates a
 * fresh directory B-tree for the filesystem root, inserts "." and ".." entries
 * and, finally, persists the root inode into the inode B-tree.
 *
 * Usage: mkfs.bsfs <partition_file>
 */

#include "../include/mkfs_bsfs.h"
#include "../include/bitmap_operations_bsfs.h"
#include "../include/btree_bsfs.h"
#include "../include/directory_operations_bsfs.h"
#include "../include/filesystem_bsfs.h"
#include "../include/inode_operations_bsfs.h"
#include "../include/superblock_operations_bsfs.h"

/**
 * @brief Prints an error message.
 *
 * @param msg Null-terminated error message to display.
 */
static void die(const char *msg) { fprintf(stderr, "mkfs.bsfs: %s\n", msg); }

/**
 * @brief Computes the ceiling of a/b for 32-bit unsigned integers.
 *
 * @param a Numerator.
 * @param b Denominator (must be > 0).
 * @return The smallest integer >= a/b.
 */
static uint32_t ceil_div_u32(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

/**
 * @brief Computes the number of bitmap blocks required to track total blocks.
 *
 * Each bitmap block stores BITMAP_BLOCK_BYTES*8 bits plus a 32-bit next pointer
 * (managed by init_bitmap_list). This computes the number of bitmap blocks
 * needed to address all blocks in the filesystem image.
 *
 * @param total_blocks Total number of blocks in the filesystem image.
 * @return Number of bitmap blocks (at least 1).
 */
static uint32_t compute_bitmap_blocks(uint32_t total_blocks) {
  const uint32_t bits_per_block = (uint32_t)(BITMAP_BLOCK_BYTES * 8u);
  uint32_t blocks = ceil_div_u32(total_blocks, bits_per_block);
  if (blocks == 0)
    blocks = 1;
  return blocks;
}

/**
 * @brief Marks metadata blocks (superblock and bitmap region) as allocated.
 *
 * Loads the bitmap, marks blocks [0 .. data_block_start-1] as in-use and
 * persists the updated bitmap back to disk.
 *
 * @param disk Opened filesystem image.
 * @param sb   Pointer to an initialized superblock with bitmap positions.
 * @return 0 on success; -1 on failure to load or write the bitmap.
 */
static int reserve_initial_blocks(FILE *disk, const superblock_t *sb) {
  // Marcar como ocupados: bloco 0 (superbloco) e todos os blocos do bitmap
  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap) {
    fprintf(stderr, "mkfs.bsfs: falha ao carregar bitmap.\n");
    return -1;
  }

  uint32_t limit = sb->data_block_start; // blocos de metadados
  if (limit > sb->total_blocks)
    limit = sb->total_blocks;

  for (uint32_t i = 0; i < limit; i++) {
    set_bit(bitmap, i);
  }

  if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
    fprintf(stderr, "mkfs.bsfs: falha ao gravar bitmap.\n");
    free(bitmap);
    return -1;
  }
  free(bitmap);
  return 0;
}

/**
 * @brief Program entry point.
 *
 * Validates the target file, computes the layout based on its size, writes the
 * superblock, initializes the bitmap linked list, reserves metadata blocks,
 * initializes the inode tree, creates the root directory tree with "." and
 * ".." entries, inserts the root inode, and prints a short summary.
 *
 * @param argc Argument count.
 * @param argv Argument vector. Requires a single path to the partition file.
 * @return EXIT_SUCCESS on success; EXIT_FAILURE on error.
 */
int mkfs_bsfs_format_open(FILE *disk) {
  if (!disk)
    return -1;

  if (fseek(disk, 0, SEEK_END) != 0) {
    perror("mkfs.bsfs: fseek");
    return -1;
  }
  long fsz = ftell(disk);
  if (fsz < 0) {
    perror("mkfs.bsfs: ftell");
    return -1;
  }
  if (fsz == 0) {
    die("empty partition file (use dd to create a sized file)");
    return -1;
  }
  if ((fsz % BLOCK_SIZE) != 0) {
    die("file size is not a multiple of block size (4096)");
    return -1;
  }

  uint32_t fs_size = (uint32_t)fsz;
  uint32_t total_blocks = fs_size / BLOCK_SIZE;
  if (total_blocks < 4) {
    die("minimum size not met (need at least 4 blocks)");
    return -1;
  }

  uint32_t bitmap_blocks = compute_bitmap_blocks(total_blocks);
  uint32_t data_start = 1 + bitmap_blocks;
  if (data_start >= total_blocks) {
    die("insufficient space for data region after metadata");
    return -1;
  }

  superblock_t sb;
  memset(&sb, 0, sizeof(sb));
  sb.magic_number = MAGIC_NUMBER;
  sb.block_size = BLOCK_SIZE;
  sb.fs_size = fs_size;
  sb.total_blocks = total_blocks;
  sb.block_bitmap_start = 1;
  sb.block_bitmap_total = bitmap_blocks;
  sb.inode_root = 0;
  sb.recovery_root = 0;
  sb.data_block_start = data_start;
  sb.root_inode = 1;

  if (write_superblock(disk, &sb) != 0) {
    die("failed to write superblock");
    return -1;
  }

  if (init_bitmap_list(disk, sb.block_bitmap_start, sb.block_bitmap_total) !=
      0) {
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
  dot.file_type = FILE_TYPE_DIR;
  dot.file_size = 0;
  strncpy(dot.filename, ".", MAX_FILENAME);
  if (btree_insert(disk, &sb, dir_handle, &dot) != 0) {
    free(dir_handle);
    die("failed to insert '.' entry into root directory");
    return -1;
  }

  dir_entry_t dotdot;
  memset(&dotdot, 0, sizeof(dotdot));
  dotdot.inode_number = sb.root_inode;
  dotdot.file_type = FILE_TYPE_DIR;
  dotdot.file_size = 0;
  strncpy(dotdot.filename, "..", MAX_FILENAME);
  if (btree_insert(disk, &sb, dir_handle, &dotdot) != 0) {
    free(dir_handle);
    die("failed to insert '..' entry into root directory");
    return -1;
  }

  inode_t root;
  memset(&root, 0, sizeof(root));
  root.inode_number = sb.root_inode;
  root.file_size = 0;
  root.file_type = FILE_TYPE_DIR;
  root.block_count = 0;
  root.uid = 0;
  root.gid = 0;
  root.permissions = 0755;
  root.links_count = 1;
  root.created_at = root.modified_at = root.accessed_at = (uint32_t)time(NULL);
  root.btree_root = dir_handle->root_block;
  for (int i = 0; i < 16; i++) {
    root.direct_blocks[i].file_blk = 0;
    root.direct_blocks[i].disk_blk = 0;
    root.direct_blocks[i].length = 0;
  }
  if (insert_inode(disk, &sb, &inode_handle, &root) != 0) {
    free(dir_handle);
    die("failed to insert root inode into inode B-tree");
    return -1;
  }

  (void)write_superblock(disk, &sb);
  free(dir_handle);

  printf("BSFS formatted successfully\n");
  printf("Partition size (bytes): %u\n", sb.fs_size);
  printf("Block size (bytes): %u\n", sb.block_size);
  printf("Total blocks: %u\n", sb.total_blocks);
  printf("Block bitmap (start,total): %u,%u\n", sb.block_bitmap_start,
         sb.block_bitmap_total);
  printf("Data block start: %u\n", sb.data_block_start);
  printf("Inode root block: %u\n", sb.inode_root);
  printf("Root inode: %u\n", sb.root_inode);

  return 0;
}

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

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <partition_file>\n", argv[0]);
    return EXIT_FAILURE;
  }
  int rc = mkfs_bsfs_format_path(argv[1]);
  return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
