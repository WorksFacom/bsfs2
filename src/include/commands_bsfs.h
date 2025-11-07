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
  char *name; /**< Command name as typed in the browser (e.g., "ls"). */
  int (*func)(FILE *file, superblock_t *sb,
              char **args); /**< Handler receiving (disk, superblock, argv-like
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

/**
 * @brief Change current working directory.
 *
 * Usage: cd &lt;directory&gt;
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
 * Usage: mkdir &lt;directory&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Directory name in args[1].
 * @return 1 to continue the REPL.
 */
int mkdir(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Remove an empty directory.
 *
 * Usage: rmdir &lt;directory&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Directory name in args[1].
 * @return 1 to continue the REPL.
 */
int rmdir(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Create an empty regular file.
 *
 * Usage: touch &lt;filename&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Target filename in args[1].
 * @return 1 to continue the REPL.
 */
int touch(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Remove a file (logical delete with Recovery snapshot).
 *
 * Unlinks the directory entry and inserts a snapshot into the Recovery tree
 * so the file can be restored later. Prints a confirmation or a concise
 * error message (including permission denied when applicable).
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Command argv-like vector; expects target path in args[1].
 * @return 1 to continue the browser loop.
 */
int rm(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Copy a file.
 *
 * Usage: cp &lt;src&gt; &lt;dst&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Source path in args[1], destination path in args[2].
 * @return 1 to continue the REPL.
 */
int cp(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Move or rename a file or directory.
 *
 * Usage: mv &lt;src&gt; &lt;dst&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Source path in args[1], destination path in args[2].
 * @return 1 to continue the REPL.
 */
int mv(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Display file contents.
 *
 * Usage: cat &lt;path&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args File path in args[1].
 * @return 1 to continue the REPL.
 */
int cat(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Write text or append text to a file, or echo to screen.
 *
 * Usage: echo "text" [&gt; file | &gt;&gt; file]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Text tokens and optional redirection operator + filename.
 * @return 1 to continue the REPL.
 */
int echo(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Show inode metadata for a path.
 *
 * Usage: stat &lt;path&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Path in args[1].
 * @return 1 to continue the REPL.
 */
int stat(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Generate a file by copying blocks from /dev/zero or /dev/random.
 *
 * Usage: dd if=/dev/zero of=&lt;file&gt; bs=&lt;size&gt; count=&lt;n&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Key-value args (if/of/bs/count) parsed by the command.
 * @return 1 to continue the REPL.
 */
int dd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Implements the fill command for the BSFS browser.
 *
 * The fill command creates a number of files with a given size, measuring the
 * elapsed time for the operation. Files are created sequentially using the
 * high-level API and filled with a predictable byte pattern.
 *
 * Usage:
 *   fill &lt;count&gt; &lt;size&gt; [prefix] [bs=&lt;block&gt;] [pattern=index|zero|random]
 *
 * Parameters:
 *   - count   : number of files to create (positive integer)
 *   - size    : file size (supports K/M/G suffixes)
 *   - prefix  : optional basename/path (default: "fill") used to build
 *               &lt;prefix&gt;_<NNN>.bin
 *   - bs=     : optional block size for writes (default 64 KiB, supports
 *               suffixes)
 *   - pattern : optional fill pattern: "index" (default, byte = file id),
 *               "zero" (all zeros) or "random" (pseudo-random bytes)
 */
int fill_cmd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Print current session uid, gid and umask.
 *
 * Reads the API session identity and umask and prints them in a compact form.
 *
 * @param disk Open filesystem image (unused).
 * @param sb   Loaded superblock (unused).
 * @param args argv-like vector (unused).
 * @return 1 to continue the browser loop.
 */
int whoami_cmd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Switch the browser session identity (testing helper).
 *
 * Changes the API session uid (and optionally gid) used for permission checks.
 * Accepts decimal values. Intended for testing permission behavior in the
 * browser; no OS-level identity is changed.
 *
 * @param disk Open filesystem image (unused).
 * @param sb   Loaded superblock (unused).
 * @param args argv-like vector; expects uid in args[1], optional gid in
 * args[2].
 * @return 1 to continue the browser loop.
 */
int su(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Show or set the session umask (octal).
 *
 * With no argument, prints the current umask. With a single octal argument,
 * sets the session umask for subsequent creates. Values must be 000..777.
 *
 * @param disk Open filesystem image (unused).
 * @param sb   Loaded superblock (unused).
 * @param args argv-like vector; optional octal value in args[1].
 * @return 1 to continue the browser loop.
 */
int umask_cmd(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Change permission bits of a path.
 *
 * Parses the octal mode and calls the high-level API to update permission
 * bits. Prints usage on invalid input and a concise error on failure.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Command argv-like vector; expects mode in args[1], path in
 * args[2].
 * @return 1 to continue the browser loop.
 */
int chmod(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Change file owner (uid) of a path (root only).
 *
 * Parses a decimal uid and updates the target's owner via the API. Prints
 * usage on invalid arguments and a concise error on failure.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Command argv-like vector; expects uid in args[1], path in
 * args[2].
 * @return 1 to continue the browser loop.
 */
int chown(FILE *disk, superblock_t *sb, char **args);

/**
 * @brief Change file group (gid) of a path (root only).
 *
 * Parses a decimal gid and updates the target's group via the API. Prints
 * usage on invalid arguments and a concise error on failure.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Command argv-like vector; expects gid in args[1], path in
 * args[2].
 * @return 1 to continue the browser loop.
 */
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
