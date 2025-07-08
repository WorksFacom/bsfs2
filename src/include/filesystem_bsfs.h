/**
 * @file filesystem_bsfs.h
 * @brief Core data structures for the BSFS (Basic Solution File System).
 *
 * This header file defines the main data structures used by the BSFS
 * file system, both on-disk and in-memory. It includes:
 *
 * - `superblock_t`: The superblock structure containing metadata about the file
 * system layout, such as block size, number of blocks, bitmap location, inode
 * roots, and root inode.
 *
 * - `bitmap_block_t`: A structure used to represent a bitmap block, including a
 * pointer to the next bitmap block for chaining.
 *
 * - `bspan_t`: Describes a logical-to-physical mapping of file blocks (block
 * spans), representing contiguous ranges of allocated disk blocks.
 *
 * - `inode_t`: Metadata structure for files and directories, including size,
 * permissions, timestamps, block mappings, and root pointers to B-trees.
 *
 * - `rec_entry_t`: A simple structure representing entries in the recovery
 * system.
 *
 * - `dir_entry_t`: Represents a directory entry, mapping filenames to inode
 * numbers.
 *
 * - `file_t`: In-memory representation of an open file, tracking seek position
 * and inode data.
 *
 * - `dir_t`: In-memory representation of an open directory, tracking iteration
 * index and inode data.
 *
 * These structures define the foundation of the BSFS layout and behavior, and
 * are used throughout the system for file management, block allocation,
 * recovery, and directory operations.
 */
#ifndef FILESYSTEM_BSFS
#define FILESYSTEM_BSFS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Magic number used to identify a BSFS-formatted file system.
 *
 * Represents the ASCII string "BSFS" encoded as hexadecimal.
 */
#define MAGIC_NUMBER 0x53465342

/**
 * @brief Size of a block in bytes.
 *
 * All operations in the file system are based on fixed-size blocks.
 */
#define BLOCK_SIZE 4096

/**
 * @brief Maximum length (in bytes) of a file or directory name.
 *
 * Does not include the null terminator.
 */
#define MAX_FILENAME 255

/**
 * @brief Size of an inode in bytes.
 *
 * Used when allocating or reading inodes from disk.
 */
#define INODE_SIZE 512

/**
 * @brief Number of 32-bit addresses that fit in a block.
 *
 * Used for indexing and pointer arrays inside blocks.
 */
#define ADDRESSES_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

/**
 * @brief Number of usable bytes for bitmap bits in a bitmap block.
 *
 * The last 4 bytes of the block are reserved for the next block pointer.
 */
#define BITMAP_BLOCK_BYTES (BLOCK_SIZE - sizeof(uint32_t))

/**
 * @brief Special value representing an invalid or null block reference.
 */
#define INVALID_BLOCK 0xFFFFFFFF

/**
 * @struct superblock_t
 * @brief Represents the superblock of the BSFS file system.
 *
 * The superblock is a fundamental structure in the BSFS file system.
 * It stores metadata about the layout and configuration of the file system
 * and is typically located at a fixed position on disk.
 */
typedef struct {
  /**
   * A constant used to validate the file system type (e.g., 0xB5F5).
   */
  uint32_t magic_number;

  /**
   * The size (in bytes) of each block in the file system.
   */
  uint32_t block_size;

  /**
   * Total size (in bytes) of the file system image or device.
   */
  uint32_t fs_size;

  /**
   * The total number of blocks available in the file system.
   */
  uint32_t total_blocks;

  /**
   * Block number where the bitmap structure begins.
   * The bitmap tracks which blocks are free or used.
   */
  uint32_t block_bitmap_start;

  /**
   * Total number of blocks used to store the bitmap.
   */
  uint32_t block_bitmap_total;

  /**
   * Block number of the root node of the B-tree that stores inode entries.
   */
  uint32_t inode_root;

  /**
   * Block number of the root node of the B-tree used for recovery records.
   */
  uint32_t recovery_root;

  /**
   * Block number where the actual data region begins (i.e., where file contents
   * are stored).
   */
  uint32_t data_block_start;

  /**
   * The inode number of the root directory of the file system.
   */
  uint32_t root_inode;
} superblock_t;

/**
 * @struct bitmap_block_t
 * @brief Represents a block of the allocation bitmap in the BSFS file system.
 *
 * This structure defines a single block used in the allocation bitmap,
 * which tracks which data blocks are free or in use. The bitmap is implemented
 * as a linked list of such blocks to support variable-length bitmaps.
 */
typedef struct {
  /**
   * A byte array representing the allocation status of a range of blocks.
   * Each bit corresponds to one block: 0 = free, 1 = allocated.
   */
  uint8_t bits[BITMAP_BLOCK_BYTES];

  /**
   * The block number of the next bitmap block in the linked list.
   * If this is the last block, this value is set to INVALID_BLOCK.
   */
  uint32_t next_block;
} bitmap_block_t;

/**
 * @struct bspan_t
 * @brief Represents a contiguous span of blocks in the BSFS file system.
 *
 * This structure maps a logical file block index to a sequence of physical
 * blocks on disk. It reduces metadata overhead by describing where file
 * data is stored when multiple blocks are allocated consecutively.
 */
typedef struct {
  /**
   * The logical block index within the file.
   * Indicates the position of this span in the file's logical address space.
   */
  uint32_t file_blk;

  /**
   * The physical block number on disk where this span begins.
   */
  uint32_t disk_blk;

  /**
   * The number of consecutive blocks starting from `disk_blk`.
   */
  uint16_t length;

  /**
   * Unused field for padding/alignment. Ensures 12-byte total size.
   */
  uint16_t padding;
} bspan_t;

/**
 * @struct inode_t
 * @brief Represents an inode in the BSFS file system.
 *
 * The inode stores metadata and block mapping information for a file or
 * directory. It has a fixed size of 256 bytes and supports both direct block
 * spans and a B-tree reference for additional data blocks.
 */
typedef struct {
  /**
   * Unique identifier for this inode in the file system.
   */
  uint32_t inode_number;

  /**
   * Total size of the file (in bytes).
   */
  uint32_t file_size;

  /**
   * File type (e.g., regular file, directory). Value is defined by an enum or
   * constant.
   */
  uint8_t file_type;

  /**
   * Number of blocks allocated to this file.
   */
  uint32_t block_count;

  /**
   * User ID of the file's owner.
   */
  uint16_t uid;

  /**
   * Group ID of the file's owner.
   */
  uint16_t gid;

  /**
   * File permissions (e.g., 0755 format for read/write/execute).
   */
  uint16_t permissions;

  /**
   * Number of hard links pointing to this inode.
   */
  uint16_t links_count;

  /**
   * Timestamp for file creation (Unix time).
   */
  uint32_t created_at;

  /**
   * Timestamp for last file modification (Unix time).
   */
  uint32_t modified_at;

  /**
   * Timestamp for last file access (Unix time).
   */
  uint32_t accessed_at;

  /**
   * Array of 16 direct block spans for small/medium file access.
   */
  bspan_t direct_blocks[16];

  /**
   * Root block number of the B-tree containing additional block spans.
   */
  uint32_t btree_root;

  /**
   * Padding bytes to ensure the structure is exactly 256 bytes.
   */
  uint8_t padding[34];
} inode_t;

/**
 * @struct rec_entry_t
 * @brief Represents a recovery entry in the BSFS file system.
 *
 * This structure is used to track entries that may be recoverable, such as
 * recently deleted or corrupted records. The recovery mechanism can use this
 * information to attempt file restoration or integrity analysis.
 *
 */
typedef struct {
  /**
   * Logical index or identifier associated with the recoverable entry.
   * This may refer to an inode number, logical block, or another
   * recovery-related ID.
   */
  uint32_t index;
} rec_entry_t;

/**
 * @struct dir_entry_t
 * @brief Represents a directory entry in the BSFS file system.
 *
 * Each directory entry maps a file or subdirectory name to its corresponding
 * inode. The structure supports variable-length entries to allow flexible
 * storage within directory blocks.
 */
typedef struct {
  /**
   * Inode number associated with the file or subdirectory.
   */
  uint32_t inode_number;

  /**
   * Total size in bytes of this directory entry, including metadata and
   * filename. Allows traversal of variable-length entries within a block.
   */
  uint16_t entry_size;

  /**
   * Number of bytes used by the filename (excluding the null terminator).
   */
  uint8_t name_size;

  /**
   * Type of file (e.g., regular file, directory, symbolic link).
   */
  uint8_t file_type;

  /**
   * Null-terminated character array storing the name of the file or directory.
   * The maximum length is defined by MAX_FILENAME.
   */
  char filename[MAX_FILENAME];
} dir_entry_t;

/**
 * @struct file_t
 * @brief Represents an open file in memory within the BSFS file system.
 *
 * This structure holds a snapshot of a file's inode and a cursor position,
 * allowing sequential or random access for read/write operations. It serves
 * as a file descriptor for open files at runtime.
 */
typedef struct {
  /**
   * The inode associated with the open file. Contains metadata such as size,
   * permissions, block pointers, and timestamps.
   */
  inode_t inode;

  /**
   * Current position (in bytes) within the file for reading or writing.
   * This field is updated by seek, read, and write operations.
   */
  uint32_t pos;
} file_t;

/**
 * @struct dir_t
 * @brief Represents an open directory in memory within the BSFS file system.
 *
 * This structure holds a snapshot of a directory's inode and a traversal index.
 * It allows for listing entries, navigating through directory contents, or
 * manipulating directory state during runtime.
 *
 *
 */
typedef struct {
  /**
   * The inode of the directory being accessed. Includes information about the
   * structure and layout of its contents.
   */
  inode_t inode;

  /**
   * Current index in the directory entry list. Used for sequential traversal
   * (e.g., readdir-like functionality).
   */
  uint32_t dir_index;
} dir_t;

#endif // FILESYSTEM_BSFS
