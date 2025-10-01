/**
 * @file browser_bsfs.h
 * @brief Public interface for the interactive BSFS browser (shell).
 *
 * Declares the main entry points used by the CLI program that allows users
 * to interact with a BSFS-formatted image. The browser reads commands from
 * standard input, tokenizes them and dispatches to the command table.
 *
 * Key responsibilities:
 *  - Read user input lines (blocking) and split into tokens
 *  - Execute commands through the registered command table
 *  - Maintain the main REPL loop while updating the displayed prompt
 */
#ifndef BROWSER_H
#define BROWSER_H

#include "commands_bsfs.h"
#include <stdio.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Superblock helpers are provided via the centralized API headers */

/**
 * @brief Read a single line from standard input.
 *
 * Allocates a buffer and returns a NUL‑terminated line suitable for tokenizing.
 * The caller is responsible for freeing the returned pointer.
 *
 * @return Newly allocated string with the input line.
 */
char *browser_read_line();

/**
 * @brief Split a command line into whitespace‑delimited tokens.
 *
 * Tokenizes the given mutable buffer in‑place and returns a NULL‑terminated
 * array of pointers to the tokens. The array itself must be freed by the
 * caller; the token strings live inside the original buffer.
 *
 * @param line Writable buffer containing the input line to split.
 * @return NULL‑terminated argv‑like vector of token pointers.
 */
char **browser_split_line(char *line);

/**
 * @brief Dispatch a parsed command to its implementation.
 *
 * Looks up the command name in the built‑in command table and invokes the
 * matching function. Unknown commands print a short error message.
 *
 * @param file Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Tokenized argv‑like vector (args[0] is the command name).
 * @return 0 to exit the REPL; 1 to continue.
 */
int browser_execute(FILE *file, superblock_t *sb, char **args);

/**
 * @brief Interactive read‑eval‑print loop (REPL) for the BSFS browser.
 *
 * Prints a prompt, reads input, tokenizes and executes commands until exit.
 *
 * @param file Open filesystem image.
 * @param sb   Loaded superblock.
 */
void browser_loop(FILE *file, superblock_t *sb);

#endif // !BROWSER_H
