/**
 * @file commands_bsfs.h
 * @brief Command table and prototypes for the BSFS browser.
 *
 * Exposes the browser command registry and individual command function
 * signatures. Each command receives the open filesystem image, the loaded
 * superblock, and the tokenized argv-like vector.
 */
#ifndef COMMANDS_H
#define COMMANDS_H

#include "api_bsfs.h"
#include <stdio.h>

/**
 * @struct Command
 * @brief Name-to-function descriptor for a browser command.
 */
typedef struct {
  char *name;
  int (*func)(FILE *file, superblock_t *sb, char **args);
} Command;

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
int bsfs_cmd_exit(FILE *file, superblock_t *sb, char **args);

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
int bsfs_cmd_info(FILE *file, superblock_t *sb, char **args);

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
int bsfs_cmd_help(FILE *file, superblock_t *sb, char **args);

/**
 * @brief Change current working directory.
 *
 * Usage: cd <directory>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Expects directory path in args[1].
 * @return 1 to continue the REPL.
 */
int cd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Print the current working directory.
 *
 * Usage: pwd
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Unused.
 * @return 1 to continue the REPL.
 */
int pwd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief List contents of a directory.
 *
 * Usage: ls [path]  or  list [path]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Optional directory path in args[1]; defaults to ".".
 * @return 1 to continue the REPL.
 */
int list(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Create a new directory.
 *
 * Usage: mkdir <directory>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Directory name in args[1].
 * @return 1 to continue the REPL.
 */
int mkdir(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Remove an empty directory.
 *
 * Usage: rmdir <directory>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Directory name in args[1].
 * @return 1 to continue the REPL.
 */
int rmdir(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Create an empty regular file.
 *
 * Usage: touch <filename>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Target filename in args[1].
 * @return 1 to continue the REPL.
 */
int touch(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Remove a file (logical delete with Recovery snapshot).
 *
 * Usage: rm <filename>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Target filename in args[1].
 * @return 1 to continue the REPL.
 */
int rm(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Copy a file.
 *
 * Usage: cp <src> <dst>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Source path in args[1], destination path in args[2].
 * @return 1 to continue the REPL.
 */
int cp(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Move or rename a file or directory.
 *
 * Usage: mv <src> <dst>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Source path in args[1], destination path in args[2].
 * @return 1 to continue the REPL.
 */
int mv(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Display file contents.
 *
 * Usage: cat <path>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args File path in args[1].
 * @return 1 to continue the REPL.
 */
int cat(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Write text or append text to a file, or echo to screen.
 *
 * Usage: echo "text" [> file | >> file]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Text tokens and optional redirection operator + filename.
 * @return 1 to continue the REPL.
 */
int echo(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Show inode metadata for a path.
 *
 * Usage: stat <path>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Path in args[1].
 * @return 1 to continue the REPL.
 */
int stat_cmd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Generate a file by copying blocks from /dev/zero or /dev/random.
 *
 * Usage: dd if=/dev/zero of=<file> bs=<size> count=<n>
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Key-value args (if/of/bs/count) parsed by the command.
 * @return 1 to continue the REPL.
 */
int dd(FILE *disk, superblock_t *sb, char **args);

/* Identity/umask commands */
int whoami_cmd(FILE *disk, superblock_t *sb, char **args);
int su_cmd(FILE *disk, superblock_t *sb, char **args);
int umask_cmd(FILE *disk, superblock_t *sb, char **args);

/* Permission metadata commands */
int chmod(FILE *disk, superblock_t *sb, char **args);
int chown(FILE *disk, superblock_t *sb, char **args);
int chgrp(FILE *disk, superblock_t *sb, char **args);

// Recovery commands

/**
 * @brief List Recovery snapshots, optionally filtered by original name.
 *
 * Usage: recovery [name]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Optional name filter in args[1].
 * @return 1 to continue the REPL.
 */
int recovery(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Restore a deleted file by Recovery ID or name.
 *
 * Usage: restore <id|name> [dest_dir]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Recovery id or name in args[1], optional destination in args[2].
 * @return 1 to continue the REPL.
 */
int restore(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Purge Recovery entries to reclaim space.
 *
 * Usage:
 *  - purge                  -> remove entries older than ~30 days (by deleted_at)
 *  - purge <id>             -> remove a specific snapshot by Recovery ID
 *  - purge oldest <count>   -> remove the <count> oldest snapshots
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Optional mode and parameters as described above.
 * @return 1 to continue the REPL.
 */
int purge(FILE *disk, superblock_t *sb, char **args);

#endif
