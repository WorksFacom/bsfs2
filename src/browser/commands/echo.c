/**
 * @file echo.c
 * @brief Implements the echo command for the BSFS filesystem.
 *
 * Writes text to files or displays text on screen.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Write text or append text to a file, or echo to screen.
 *
 * Usage: echo "text" [&gt; file | &gt;&gt; file]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Text tokens and optional redirection operator + filename.
 * @return 1 to continue the REPL.
 */
int echo(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: echo <text> [> filename | >> filename]\n");
    return 1;
  }

  // Find redirection operator
  int redirect_pos = -1;
  int append_mode = 0;

  for (int i = 1; args[i]; i++) {
    if (strcmp(args[i], ">") == 0) {
      redirect_pos = i;
      append_mode = 0;
      break;
    } else if (strcmp(args[i], ">>") == 0) {
      redirect_pos = i;
      append_mode = 1;
      break;
    }
  }

  // Build text to echo
  char text[1024] = "";
  int text_end = (redirect_pos > 0) ? redirect_pos : 0;

  for (int i = 1; args[i] && (text_end == 0 || i < text_end); i++) {
    if (i > 1)
      strcat(text, " ");
    strcat(text, args[i]);
  }
  strcat(text, "\n");

  // No redirection - print to screen
  if (redirect_pos == -1) {
    printf("%s", text);
    return 1;
  }

  // Check if filename is provided
  if (!args[redirect_pos + 1]) {
    printf("Error: No filename specified after redirection operator\n");
    return 1;
  }

  const char *filename = args[redirect_pos + 1];

  // Handle file redirection
  if (!append_mode) {
    // Overwrite mode: if file does not exist, create it; then truncate and
    // write
    inode_t inode;
    if (stat_bsfs(disk, sb, filename, &inode) != 0) {
      if (create_file_bsfs(disk, sb, filename) != 0) {
        if (errno == EACCES)
          printf("Permission denied\n");
        else
          printf("Error creating file: %s\n", filename);
        return 1;
      }
    }
    size_t n = overwrite_bsfs(disk, sb, filename, text, (uint32_t)strlen(text));
    if (n != strlen(text)) {
      if (errno == EACCES)
        printf("Permission denied\n");
      else
        printf("Error writing to file: %s\n", filename);
      return 1;
    }
    printf("Text written to file: %s\n", filename);
    return 1;
  }

  // Append mode: open, seek to end, write
  file_t file;
  if (open_bsfs(disk, sb, filename, &file) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error opening file: %s\n", filename);
    return 1;
  }

  // Get file size and seek to end
  inode_t inode;
  if (stat_bsfs(disk, sb, filename, &inode) == 0) {
    seek_bsfs(&file, inode.file_size);
  }

  size_t bytes_written = write_bsfs(disk, sb, &file, text, strlen(text));
  if (bytes_written != strlen(text)) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error writing to file: %s\n", filename);
    close_bsfs(&file);
    return 1;
  }

  close_bsfs(&file);
  printf("Text written to file: %s\n", filename);
  return 1;
}
