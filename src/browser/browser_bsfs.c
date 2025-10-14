/**
 * @file browser_bsfs.c
 * @brief Browser shell-like for the BSFS file system.
 *
 * This program allows interaction with a BSFS-formatted file through a
 * shell-like interface. Usage: ./browser &lt;formatted_file&gt; It validates
 * the file, reads the superblock, and enters a command loop.
 */
#include "../include/browser_bsfs.h"
#include "../include/api_bsfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char current_path[PATH_MAX] = "/"; // Initialize current path to root

/* Use shared read_superblock from superblock_operations_bsfs.c */

/**
 * @brief Read a single line from standard input.
 *
 * Allocates a buffer and returns a NUL-terminated line suitable for tokenizing.
 * The caller is responsible for freeing the returned pointer.
 *
 * @return Newly allocated string with the input line.
 */
char *browser_read_line() {
  char *line = NULL;
  size_t bufsize = 0;

  if (getline(&line, &bufsize, stdin) == -1) {
    if (feof(stdin)) {
      exit(EXIT_SUCCESS);
    } else {
      perror("readline");
      exit(EXIT_FAILURE);
    }
  }
  return line;
}

/**
 * @brief Split a command line into whitespace-delimited tokens.
 *
 * Tokenizes the given mutable buffer in-place and returns a NULL-terminated
 * array of pointers to the tokens. The array itself must be freed by the
 * caller; the token strings live inside the original buffer.
 *
 * @param line Writable buffer containing the input line to split.
 * @return NULL-terminated argv-like vector of token pointers.
 */
char **browser_split_line(char *line) {
  int bufsize = 64;
  int position = 0;
  char **tokens = malloc(bufsize * sizeof(char));
  char *token;

  if (!tokens) {
    fprintf(stderr, "BSFS browser: allocation error\n");
    exit(EXIT_FAILURE);
  }

  token = strtok(line, " \t\r\n\a");
  while (token != NULL) {
    tokens[position] = token;
    position++;

    if (position >= bufsize) {
      bufsize += 64;
      tokens = realloc(tokens, bufsize * sizeof(char *));
      if (!tokens) {
        fprintf(stderr, "BSFS browser: allocation error\n");
        exit(EXIT_FAILURE);
      }
    }

    token = strtok(NULL, " \t\r\n\a");
  }
  tokens[position] = NULL;
  return tokens;
}

/**
 * @brief Dispatch a parsed command to its implementation.
 *
 * Looks up the command name in the built-in command table and invokes the
 * matching function. Unknown commands print a short error message.
 *
 * @param file Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Tokenized argv-like vector (args[0] is the command name).
 * @return 0 to exit the REPL; 1 to continue.
 */
int browser_execute(FILE *file, superblock_t *sb, char **args) {
  if (args[0] == NULL) {
    return 1;
  }

  Command commands[] = {{"exit", bsfs_cmd_exit},
                        {"info", bsfs_cmd_info},
                        {"help", bsfs_cmd_help},
                        {"cd", cd},
                        {"pwd", pwd},
                        {"whoami", whoami_cmd},
                        {"su", su},
                        {"umask", umask_cmd},
                        {"ls", list},
                        {"list", list},
                        {"recovery", recovery},
                        {"restore", restore},
                        {"purge", purge},
                        {"mkdir", mkdir},
                        {"rmdir", rmdir},
                        {"touch", touch},
                        {"rm", rm},
                        {"cp", cp},
                        {"mv", mv},
                        {"cat", cat},
                        {"fill", fill_cmd},
                        {"chmod", chmod},
                        {"chown", chown},
                        {"chgrp", chgrp},
                        {"echo", echo},
                        {"stat", stat},
                        {"dd", dd}};

  int num_commands = sizeof(commands) / sizeof(Command);

  for (int i = 0; i < num_commands; i++) {
    if (strcmp(args[0], commands[i].name) == 0) {
      return commands[i].func(file, sb, args);
    }
  }

  fprintf(stderr, "BSFS: unknown command: %s\n", args[0]);
  return 1;
}

/**
 * @brief Interactive read-eval-print loop (REPL) for the BSFS browser.
 *
 * Prints a prompt, reads input, tokenizes and executes commands until exit.
 *
 * @param file Open filesystem image.
 * @param sb   Loaded superblock.
 */
void browser_loop(FILE *file, superblock_t *sb) {
  char *line;
  char **args;
  int status;
  do {
    // Refresh superblock from disk so commands see up-to-date roots
    read_superblock(file, sb);
    // Sync prompt path with API's current working directory
    (void)getcwd_bsfs(file, sb, current_path, sizeof(current_path));
    fprintf(stdout, "BSFS:%s> ", current_path);
    line = browser_read_line();
    args = browser_split_line(line);
    status = browser_execute(file, sb, args);

    free(line);
    free(args);
  } while (status);
}

/**
 * @brief Program entry point for the BSFS browser.
 *
 * Validates usage, opens the formatted image file, checks the magic number,
 * reads the superblock and starts the interactive loop.
 *
 * @param argc Argument count (expects 2).
 * @param argv Argument vector: argv[1] must be the path to the image file.
 * @return EXIT_SUCCESS on normal termination; EXIT_FAILURE on error.
 */
int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <formatted file>\n", argv[0]);
    return EXIT_FAILURE;
  }
  FILE *file = fopen(argv[1], "rb+");
  if (!file) {
    perror("Error opening file");
    return EXIT_FAILURE;
  }

  fseek(file, 0, SEEK_SET);
  uint32_t magic_number;
  fread(&magic_number, sizeof(uint32_t), 1, file);
  if (magic_number != MAGIC_NUMBER) {
    perror("Invalid file");
    return EXIT_FAILURE;
  }

  superblock_t superblock = {0};
  read_superblock(file, &superblock);
  fprintf(stdout,
          "BSFS Browser\nPartition: %s\nType \"help\" for available commands "
          "or \"info\" for filesystem details\n",
          argv[1]);

  browser_loop(file, &superblock);

  return EXIT_SUCCESS;
}
