/**
 * @file mkfs.c
 * @brief mkfs.bsfs, formats an unmounted image or block device.
 *
 * builds the root directory and inode trees through the same libbsfs txn recovery.bsfs restores with,
 * so the layout and ordering come from the shared code. the old superblock is wiped first and the new
 * one written last, after everything it points at is synced.
 */
#include "libbsfs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int add_dentry(struct bsfs_txn *tx, uint32_t *root, const char *name, uint32_t ino) {
  struct tree_desc dir_tree = bsfs_dir_tree();
  bsfs_dentry      d        = {.ino = ino};

  strcpy(d.filename, name);
  d.name_hash = hash_filename(d.filename);
  return tree_insert(tx, &dir_tree, root, &d);
}

static int format(int fd) {
  struct tree_desc    inode_tree = bsfs_inode_tree();
  struct bitmap_state bitmap     = {0};
  bsfs_super_block    sb         = {0};
  struct bsfs_txn     tx         = {.fd = fd, .sb = &sb, .bitmap = &bitmap};
  disk_mapped_inode_t root       = {0};
  uint8_t             zero[BSFS_BLOCK_SIZE] = {0};
  uint64_t            bytes, total_blocks;
  uint32_t            bitmap_blocks, data_start, dir_root = 0, inode_root = 0;
  int                 rc = -1;

  if (bsfs_dev_size(fd, &bytes) != 0) {
    perror("mkfs.bsfs: size");
    return -1;
  }
  if (bytes == 0) {
    fprintf(stderr, "mkfs.bsfs: empty partition file (use dd to create a sized file)\n");
    return -1;
  }
  if (bytes % BSFS_BLOCK_SIZE) {
    bytes -= bytes % BSFS_BLOCK_SIZE;
    fprintf(stderr, "mkfs.bsfs: warning: size not multiple of %u; truncating to %" PRIu64 " bytes\n", BSFS_BLOCK_SIZE, bytes);
  }
  total_blocks = bytes / BSFS_BLOCK_SIZE;
  if (total_blocks > 0xFFFFFFFFULL) {
    fprintf(stderr, "mkfs.bsfs: filesystem too large for 32-bit block addressing (limit ~16 TiB at 4K)\n");
    return -1;
  }

  bitmap_blocks = (uint32_t)((total_blocks + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK);
  data_start    = 2 + bitmap_blocks;
  // one leaf each for the root dir and the inode tree
  if (total_blocks < (uint64_t)data_start + 2) {
    fprintf(stderr, "mkfs.bsfs: minimum size not met (need at least %u blocks)\n", data_start + 2);
    return -1;
  }

  // a crash from here on leaves no magic rather than an old superblock over new metadata
  if (bsfs_write_block(fd, 0, zero) != 0 || bsfs_write_block(fd, 1, zero) != 0) {
    perror("mkfs.bsfs: wipe superblock");
    return -1;
  }

  sb.magic_number       = MAGIC_NUMBER;
  sb.block_size         = BSFS_BLOCK_SIZE;
  sb.fs_size            = bytes;
  sb.total_blocks       = (uint32_t)total_blocks;
  sb.block_bitmap_start = 2;
  sb.block_bitmap_total = bitmap_blocks;
  sb.data_block_start   = data_start;
  sb.root_inode         = BSFS_ROOT_INO;

  if (bitmap_create(&sb, &bitmap) != 0) {
    fprintf(stderr, "mkfs.bsfs: out of memory\n");
    return -1;
  }
  for (uint32_t i = 0; i < data_start; i++)
    (void)bitmap_set(&bitmap, i, 1);

  if (add_dentry(&tx, &dir_root, ".", BSFS_ROOT_INO) != 0 || add_dentry(&tx, &dir_root, "..", BSFS_ROOT_INO) != 0) {
    fprintf(stderr, "mkfs.bsfs: failed to build the root directory tree\n");
    goto out;
  }

  root.ino        = BSFS_ROOT_INO;
  root.mode       = (uint16_t)(S_IFDIR | 0755);
  root.nlinks     = 2;
  root.atime      = (uint32_t)time(NULL);
  root.mtime      = root.atime;
  root.ctime      = root.atime;
  root.btree_root = dir_root;
  if (tree_insert(&tx, &inode_tree, &inode_root, &root) != 0) {
    fprintf(stderr, "mkfs.bsfs: failed to build the inode tree\n");
    goto out;
  }

  sb.inode_root = inode_root;
  if (txn_commit(&tx, &sb) != 0)
    goto out;

  printf("BSFS formatted successfully\n");
  printf("Partition size (bytes): %" PRIu64 "\n", sb.fs_size);
  printf("Block size (bytes): %u\n", sb.block_size);
  printf("Total blocks: %u\n", sb.total_blocks);
  printf("Block bitmap (start,total): %u,%u\n", sb.block_bitmap_start, sb.block_bitmap_total);
  printf("Data block start: %u\n", sb.data_block_start);
  printf("Inode root block: %u\n", sb.inode_root);
  printf("Dir entry root block: %u\n", dir_root);
  printf("Root inode: %u\n", sb.root_inode);
  rc = 0;

out:
  txn_destroy(&tx);
  bitmap_free(&bitmap);
  return rc;
}

/** @brief formats the target in argv[1]. EXIT_SUCCESS, or EXIT_FAILURE having printed why. */
int main(int argc, char **argv) {
  int fd, rc;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <partition_file>\n", argv[0]);
    return EXIT_FAILURE;
  }
  fd = bsfs_dev_open(argv[1], 1);
  if (fd < 0)
    return EXIT_FAILURE;
  rc = format(fd);
  close(fd);
  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
