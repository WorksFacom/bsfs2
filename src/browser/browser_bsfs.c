/**
 * @file browser.c
 * @brief Browser shell-like for the BSFS file system.
 *
 * This program allows interaction with a BSFS-formatted file through a
 * shell-like interface. Usage: ./browser <formatted_file> It validates the
 * file, reads the superblock, and enters a command loop.
 */
#include "../include/browser_bsfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char current_path[PATH_MAX] = "/"; // Initialize current path to root

/**
 * @brief Reads the superblock from the file and fills the superblock_t struct.
 * @param file Pointer to the opened BSFS file.
 * @param sb Pointer to the superblock_t struct to be filled.
 */
void read_superblock(FILE *file, superblock_t *sb) {
  fseek(file, 0, SEEK_SET);
  fread(sb, sizeof(superblock_t), 1, file);
  fflush(file);
}

/**
 * @brief Reads a line from standard input.
 * @return A pointer to the dynamically allocated line string.
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
 * @brief Splits a line into tokens (arguments).
 * @param line The input line to split.
 * @return An array of token strings, NULL-terminated.
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
 * @brief Executes a command by looking it up in the command table.
 * @param file Pointer to the BSFS file.
 * @param sb Superblock struct with file system metadata.
 * @param args Tokenized arguments from user input.
 * @return 0 to exit, 1 to continue loop.
 */
int browser_execute(FILE *file, superblock_t sb, char **args) {
  if (args[0] == NULL) {
    return 1;
  }

  Command commands[] = {{"exit", bsfs_cmd_exit},
                        {"info", bsfs_cmd_info},
                        {"help", bsfs_cmd_help},
                        {"cd", cd},
                        {"pwd", pwd},
                        {"ls", list},
                        {"list", list},
                        {"mkdir", mkdir},
                        {"rmdir", rmdir},
                        {"touch", touch},
                        {"rm", rm},
                        {"cp", cp},
                        {"mv", mv},
                        {"cat", cat},
                        {"echo", echo},
                        {"stat", stat_cmd}};

  int num_commands = sizeof(commands) / sizeof(Command);

  for (int i = 0; i < num_commands; i++) {
    if (strcmp(args[0], commands[i].name) == 0) {
      return commands[i].func(file, &sb, args);
    }
  }

  fprintf(stderr, "BSFS: unknown command: %s\n", args[0]);
  return 1;
}

/**
 * @brief Main loop of the browser shell.
 * Continuously reads and executes commands.
 * @param file Pointer to the BSFS file.
 * @param sb Pointer to the loaded superblock.
 */
void browser_loop(FILE *file, superblock_t *sb) {
  char *line;
  char **args;
  int status;
  do {
    fprintf(stdout, "BSFS:%s> ", current_path);
    line = browser_read_line();
    args = browser_split_line(line);
    status = browser_execute(file, *sb, args);

    free(line);
    free(args);
  } while (status);
}

/**
 * @brief Main function for the BSFS browser program.
 * Validates the file, loads the superblock, and starts the browser loop.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
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