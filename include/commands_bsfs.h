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

/**
 * @struct Command
 * @brief Name-to-function descriptor for a browser command.
 */
typedef struct {
  char *name;                                             /**< Command name as typed in the browser (e.g., "ls"). */
  int (*func)(FILE *file, superblock_t *sb, char **args); /**< Handler receiving (disk, superblock, argv-like
                                                             args); returns 0 to exit, 1 to continue. */
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
 * @brief Restore a deleted file by recovery ID or original name.
 *
 * If the first argument parses as a numeric ID, restores that snapshot.
 * Otherwise treats it as a name and disambiguates if multiple snapshots share
 * the same name, printing a table of candidates with deletion timestamps.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args argv-like vector; id or name in args[1], optional dest dir in
 * args[2].
 * @return 1 to continue the browser loop.
 */
int restore(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief BSFS browser command: purge Recovery snapshots to reclaim space.
 *
 * Usage:
 *  - purge                  -> remove entries older than ~30 days
 *  - purge &lt;id&gt;             -> remove a specific snapshot by Recovery ID
 *  - purge oldest &lt;count&gt;   -> remove the &lt;count&gt; oldest snapshots
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Tokenized arguments per usage above.
 * @return Always 1 to continue the browser loop.
 */
int purge(FILE *disk, superblock_t *sb, char **args);

#endif
