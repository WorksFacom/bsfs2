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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @brief Magic number used to identify a BSFS-formatted file system.
 *
 * Represents the ASCII string "BSFS" encoded as hexadecimal.
 */
/** Magic number used to identify a BSFS image ("BSFS" in hex). */
#define MAGIC_NUMBER 0x53465342

/**
 * @brief Size of a block in bytes.
 *
 * All operations in the file system are based on fixed-size blocks.
 */
/** Logical block size in bytes. */
#define BLOCK_SIZE 4096

/**
 * @brief Maximum length (in bytes) of a file or directory name.
 *
 * Does not include the null terminator.
 */
/** Maximum filename length (bytes, excluding NUL). */
#define MAX_FILENAME 255

/**
 * @brief Size of an inode in bytes.
 *
 * Used when allocating or reading inodes from disk.
 */
/** Size of an inode on disk (bytes). */
#define INODE_SIZE 512

/**
 * @brief Number of 32-bit addresses that fit in a block.
 *
 * Used for indexing and pointer arrays inside blocks.
 */
/** Helper: number of 32-bit addresses per block. */
#define ADDRESSES_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t))

/**
 * @brief Number of usable bytes for bitmap bits in a bitmap block.
 *
 * The last 4 bytes of the block are reserved for the next block pointer.
 */
/** Usable bytes in a bitmap block (reserve 4B for next pointer). */
#define BITMAP_BLOCK_BYTES (BLOCK_SIZE - sizeof(uint32_t))

/**
 * @brief Special value representing an invalid or null block reference.
 */
/** Sentinel for an invalid or absent block address. */
#define INVALID_BLOCK 0xFFFFFFFF

/** File type code: directory. */
#define FILE_TYPE_DIR 1
/** File type code: regular file. */
#define FILE_TYPE_FILE 2
/** Default retention period for recovery entries (seconds). */
#define DEFAULT_RECOVERY_TTL (30ULL * 24ULL * 60ULL * 60ULL)

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
   *
   * 64-bit to support large devices/images beyond 4 GiB.
   */
  uint64_t fs_size;

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
 * directory. It has a fixed size of 512 bytes and supports both direct block
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
 * @brief Represents a deleted file entry stored in the recovery B-tree.
 *
 * Each recovery entry acts as a snapshot of a file's inode at the moment it
 * was deleted. These entries enable the filesystem to restore deleted files
 * without immediately freeing their data blocks. Recovery entries are stored
 * in a dedicated B-tree, separate from the inode tree, and remain valid until
 * they are purged manually or by retention policies.
 */
typedef struct {
  /**
   * Unique identifier for this recovery entry, assigned sequentially or
   * uniquely at deletion time. Used by user-facing recovery commands
   * (e.g., listing and restoring deleted files).
   */
  uint32_t recovery_id;

  /**
   * Timestamp of when the file was deleted, in Unix time (seconds since epoch).
   * Used for listing recovery entries chronologically and for retention checks.
   */
  uint64_t deleted_at;

  /**
   * Inode number of the original parent directory where the file resided.
   * Useful for restoring the file to its original location, if available.
   */
  uint32_t original_parent_ino;

  /**
   * Original file name at deletion time. Used when restoring the file or
   * displaying it in recovery listings.
   */
  char original_name[256];

  /**
   * Inode number of the deleted file. Can be reused when restoring the file
   * to simplify consistency with previous references.
   */
  uint32_t inode_number;

  /**
   * Type of the file at deletion (e.g., regular file, directory, symlink).
   * Uses the same enum/constant values as @ref inode_t::file_type.
   */
  uint8_t file_type;

  /**
   * User ID of the file owner at deletion time.
   */
  uint16_t uid;

  /**
   * Group ID of the file owner at deletion time.
   */
  uint16_t gid;

  /**
   * File permissions (e.g., 0755 format), copied from the inode at deletion.
   */
  uint16_t permissions;

  /**
   * Total logical size of the file in bytes at deletion time.
   */
  uint32_t file_size;

  /**
   * Number of blocks allocated to the file at deletion time.
   */
  uint32_t block_count;

  /**
   * Snapshot of the inode's 16 direct block spans, pointing to the physical
   * blocks used by the file. Preserved to allow block reuse during restoration.
   */
  bspan_t direct_blocks[16];

  /**
   * Root block number of the B-tree containing additional block spans for
   * large files. Preserved so that the full file data mapping can be recovered.
   */
  uint32_t btree_root;

  /**
   * Timestamp until which this recovery entry is retained, in Unix time.
   * After this point, a garbage collector may safely purge the entry and
   * release its data blocks back to the free space pool.
   */
  uint64_t retention_until;
} rec_entry_t;

/**
 * @struct dir_entry_t
 * @brief Represents a directory entry in the BSFS file system.
 *
 * Each directory entry maps a file or subdirectory name to its corresponding
 * inode.
 */
typedef struct {
  /**
   * Inode number associated with the file or subdirectory.
   */
  uint32_t inode_number;

  /**
   * Type of file (e.g., regular file, directory, symbolic link).
   */
  uint8_t file_type;

  /**
   * Size of the file in bytes (as last known when directory was written).
   * Used to optimize operations like `ls` without reading full inode.
   */
  uint8_t file_size;

  /**
   * Hash value of the filename, used for indexing or fast lookup.
   */
  uint32_t name_hash;

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

#endif // FILESYSTEM_BSFS
