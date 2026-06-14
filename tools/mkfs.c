#include "../include/bsfs_shared.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/bsfs_shared.h"

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, uint64_t)
#endif

#define BSFS_MODE_DIR 0040000

static void die(const char *msg) {
  fprintf(stderr, "mkfs.bsfs: %s\n", msg);
}

static uint64_t ceil_div_u64(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

static int write_block(FILE *disk, uint32_t block_num, const void *data) {
  if (fseek(disk, (off_t)block_num * BSFS_BLOCK_SIZE, SEEK_SET) != 0)
    return -1;
  if (fwrite(data, BSFS_BLOCK_SIZE, 1, disk) != 1)
    return -1;
  return 0;
}

static void bsfs_set_bit(uint8_t *bitmap, uint32_t index) {
  bitmap[index >> 3] |= (uint8_t)(1U << (index & 7));
}

static uint32_t compute_bitmap_blocks(uint64_t total_blocks) {
  uint32_t bits_per_block = BITS_PER_BLOCK;
  uint32_t blocks         = (uint32_t)ceil_div_u64(total_blocks, bits_per_block);
  if (blocks == 0)
    blocks = 1;
  return blocks;
}

static int init_bitmap_list(FILE *disk, uint32_t start_block, uint32_t num_blocks) {
  for (uint32_t i = 0; i < num_blocks; i++) {
    bitmap_block_t block;
    memset(&block, 0, sizeof(block));
    if (i < num_blocks - 1)
      block.next_block = start_block + i + 1;
    else
      block.next_block = INVALID_BLOCK;
    if (write_block(disk, start_block + i, &block) != 0) {
      fprintf(stderr, "mkfs.bsfs: failed to write bitmap block %u\n", start_block + i);
      return -1;
    }
  }
  return 0;
}

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
  if ((fsz % BSFS_BLOCK_SIZE) != 0) {
    uint64_t aligned = fsz - (fsz % BSFS_BLOCK_SIZE);
    if (aligned < (uint64_t)BSFS_BLOCK_SIZE * 4) {
      die("file size is not a multiple of block size (4096)");
      return -1;
    }
    fprintf(stderr, "mkfs.bsfs: warning: size not multiple of %u; truncating to %" PRIu64 " bytes\n", BSFS_BLOCK_SIZE, aligned);
    fsz = aligned;
  }

  uint64_t fs_size      = fsz;
  uint64_t total_blocks = fs_size / BSFS_BLOCK_SIZE;
  if (total_blocks < 4) {
    die("minimum size not met (need at least 4 blocks)");
    return -1;
  }
  if (total_blocks > 0xFFFFFFFFULL) {
    die("filesystem too large for 32-bit block addressing (limit ~16 TiB at 4K)");
    return -1;
  }

  uint32_t bitmap_blocks = compute_bitmap_blocks(total_blocks);
  uint32_t data_start    = 2 + bitmap_blocks;

  if (data_start >= total_blocks) {
    die("insufficient space for data region after metadata");
    return -1;
  }

  /* Allocate btree root blocks from the data region */
  uint32_t dir_root_block   = data_start;
  uint32_t inode_root_block = data_start + 1;

  if (inode_root_block >= total_blocks) {
    die("insufficient space for B-tree roots");
    return -1;
  }

  {
    uint8_t zero[BSFS_BLOCK_SIZE] = {0};
    if (write_block(disk, 0, zero) != 0) {
      die("failed to write reserved block 0");
      return -1;
    }
  }

  //  Write superblock at block 1
  bsfs_super_block sb;
  memset(&sb, 0, sizeof(sb));
  sb.magic_number       = MAGIC_NUMBER;
  sb.block_size         = BSFS_BLOCK_SIZE;
  sb.fs_size            = fs_size;
  sb.total_blocks       = (uint32_t)total_blocks;
  sb.block_bitmap_start = 2;
  sb.block_bitmap_total = bitmap_blocks;
  sb.inode_root         = inode_root_block;
  sb.recovery_root      = 0;
  sb.data_block_start   = data_start;
  sb.root_inode         = BSFS_ROOT_INO;

  if (write_block(disk, 1, &sb) != 0) {
    die("failed to write superblock");
    return -1;
  }

  //  Initialize bitmap linked list
  if (init_bitmap_list(disk, sb.block_bitmap_start, sb.block_bitmap_total) != 0) {
    die("failed to initialize bitmap list");
    return -1;
  }

  // Reserve all metadata blocks in bitmap
  {
    size_t   bitmap_bytes = (size_t)bitmap_blocks * BITMAP_BLOCK_BYTES;
    uint8_t *bitmap       = calloc(1, bitmap_bytes);
    if (!bitmap) {
      die("out of memory");
      return -1;
    }

    uint32_t limit = inode_root_block + 1;
    if (limit > sb.total_blocks)
      limit = sb.total_blocks;
    for (uint32_t i = 0; i < limit; i++)
      bsfs_set_bit(bitmap, i);

    for (uint32_t i = 0; i < bitmap_blocks; i++) {
      bitmap_block_t block;
      memset(&block, 0, sizeof(block));
      memcpy(block.bits, bitmap + (size_t)i * BITMAP_BLOCK_BYTES, BITMAP_BLOCK_BYTES);
      block.next_block = (i < bitmap_blocks - 1) ? sb.block_bitmap_start + i + 1 : INVALID_BLOCK;
      if (write_block(disk, sb.block_bitmap_start + i, &block) != 0) {
        fprintf(stderr, "mkfs.bsfs: failed to write bitmap block %u\n", sb.block_bitmap_start + i);
        free(bitmap);
        return -1;
      }
    }
    free(bitmap);
  }

  // Create directory entry B-tree root (leaf, 2 entries: "." and "..")
  {
    btree_node_t node;
    memset(&node, 0, sizeof(node));
    node.is_leaf     = 1;
    node.num_entries = 2;

    bsfs_dentry dot;
    memset(&dot, 0, sizeof(dot));
    dot.ino       = BSFS_ROOT_INO;
    dot.name_hash = hash_filename(".");
    strncpy(dot.filename, ".", MAX_FILENAME);

    bsfs_dentry dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    dotdot.ino       = BSFS_ROOT_INO;
    dotdot.name_hash = hash_filename("..");
    strncpy(dotdot.filename, "..", MAX_FILENAME);

    memcpy(node.data, &dot, sizeof(dot));
    memcpy(node.data + sizeof(dot), &dotdot, sizeof(dotdot));

    if (write_block(disk, dir_root_block, &node) != 0) {
      die("failed to write directory B-tree root");
      return -1;
    }
  }

  // Create inode B-tree root (leaf, 1 entry: root inode)
  {
    btree_node_t node;
    memset(&node, 0, sizeof(node));
    node.is_leaf     = 1;
    node.num_entries = 1;

    uint32_t now = (uint32_t)time(NULL);

    disk_mapped_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.ino         = BSFS_ROOT_INO;
    root_inode.size        = 0;
    root_inode.unused      = 0;
    root_inode.block_count = 0;
    root_inode.uid         = 0;
    root_inode.gid         = 0;
    root_inode.mode        = (uint16_t)(BSFS_MODE_DIR | 0755);
    root_inode.nlinks      = 2;
    root_inode.atime       = now;
    root_inode.mtime       = now;
    root_inode.ctime       = now;
    root_inode.btree_root  = dir_root_block;

    memcpy(node.data, &root_inode, sizeof(root_inode));

    if (write_block(disk, inode_root_block, &node) != 0) {
      die("failed to write inode B-tree root");
      return -1;
    }
  }

  printf("BSFS formatted successfully\n");
  printf("Partition size (bytes): %" PRIu64 "\n", sb.fs_size);
  printf("Block size (bytes): %u\n", sb.block_size);
  printf("Total blocks: %u\n", sb.total_blocks);
  printf("Block bitmap (start,total): %u,%u\n", sb.block_bitmap_start, sb.block_bitmap_total);
  printf("Data block start: %u\n", sb.data_block_start);
  printf("Inode root block: %u\n", sb.inode_root);
  printf("Dir entry root block: %u\n", dir_root_block);
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
