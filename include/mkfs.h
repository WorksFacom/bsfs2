/**
 * @file mkfs_bsfs.h
 * @brief Public interface and documentation for the BSFS formatter tool.
 *
 * This header documents the formatter utility that creates a BSFS filesystem
 * image over a regular file (e.g., produced by `dd`). The formatter writes the
 * superblock, initializes the block bitmap as a linked list of bitmap blocks,
 * reserves metadata blocks, initializes the inode B-tree, creates a fresh
 * root directory B-tree with "." and ".." entries, and persists the root
 * inode (number 1 by convention).
 *
 * The canonical binary built by this project is `mkfs.bsfs`, which accepts the
 * path to a partition file as its single argument.
 *
 * Example:
 *  - dd if=/dev/zero of=bsfs.img bs=1M count=10
 *  - ./bin/mkfs.bsfs bsfs.img
 */

#ifndef MKFS_BSFS_H
#define MKFS_BSFS_H

#include <stdio.h>

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
int mkfs_bsfs_format_open(FILE *disk);

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
int mkfs_bsfs_format_path(const char *path);

#endif // MKFS_BSFS_H
