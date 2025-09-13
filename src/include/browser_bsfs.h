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
char *browser_read_line();
char **browser_split_line(char *line);
int browser_execute(FILE *file, superblock_t *sb, char **args);
void browser_loop(FILE *file, superblock_t *sb);

#endif // !BROWSER_H
