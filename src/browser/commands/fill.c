/**
 * @file fill.c
 * @brief Implements the fill command for the BSFS browser.
 *
 * The fill command creates a number of files with a given size, measuring the
 * elapsed time for the operation. Files are created sequentially using the
 * high-level API and filled with a predictable byte pattern.
 *
 * Usage:
 *   fill <count> <size> [prefix] [bs=<block>] [pattern=index|zero|random]
 *
 * Parameters:
 *   - count   : number of files to create (positive integer)
 *   - size    : file size (supports K/M/G suffixes)
 *   - prefix  : optional basename/path (default: "fill") used to build
 *               <prefix>_<NNN>.bin
 *   - bs=     : optional block size for writes (default 64 KiB, supports
 *               suffixes)
 *   - pattern : optional fill pattern: "index" (default, byte = file id),
 *               "zero" (all zeros) or "random" (pseudo-random bytes)
 */

#include "../../include/api_bsfs.h"
#include "../../include/commands_bsfs.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>

/// Current browser prompt path (defined in browser_bsfs.c)
extern char current_path[];

#define FILL_DEFAULT_BLOCK (64 * 1024)

/**
 * @brief Obtain a monotonic timestamp in seconds.
 */
static double monotonic_seconds(void) {
#ifdef CLOCK_MONOTONIC
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#else
  return (double)clock() / CLOCKS_PER_SEC;
#endif
}

/**
 * @brief Parse a size string that may contain K/M/G suffixes.
 *
 * @param text Input text (e.g., "1024", "4K", "10M").
 * @param out_bytes Output: parsed size in bytes (uint32_t).
 * @return 0 on success, -1 on error.
 */
static int parse_size_arg(const char *text, uint32_t *out_bytes) {
  if (!text || !*text || !out_bytes)
    return -1;

  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0)
    return -1;

  unsigned long long multiplier = 1;
  if (end && *end != '\0') {
    if (*(end + 1) != '\0')
      return -1;
    switch (*end) {
    case 'K':
    case 'k':
      multiplier = 1024ULL;
      break;
    case 'M':
    case 'm':
      multiplier = 1024ULL * 1024ULL;
      break;
    case 'G':
    case 'g':
      multiplier = 1024ULL * 1024ULL * 1024ULL;
      break;
    default:
      return -1;
    }
  }

  unsigned long long bytes = value * multiplier;
  if (bytes == 0 || bytes > UINT32_MAX)
    return -1;

  *out_bytes = (uint32_t)bytes;
  return 0;
}

/**
 * @brief Resolve a potentially relative path against the browser CWD.
 */
static int resolve_path(const char *input, char *absolute, size_t cap) {
  if (!input || !absolute || cap == 0)
    return -1;

  if (input[0] == '/') {
    size_t len = strlen(input);
    if (len >= cap)
      return -1;
    memcpy(absolute, input, len + 1);
    return 0;
  }

  size_t cwd_len = strlen(current_path);
  size_t name_len = strlen(input);

  if (strcmp(current_path, "/") == 0) {
    if (1 + name_len >= cap)
      return -1;
    absolute[0] = '/';
    memcpy(absolute + 1, input, name_len + 1);
  } else {
    if (cwd_len + 1 + name_len >= cap)
      return -1;
    memcpy(absolute, current_path, cwd_len);
    absolute[cwd_len] = '/';
    memcpy(absolute + cwd_len + 1, input, name_len + 1);
  }
  return 0;
}

/**
 * @brief Fill command implementation.
 */
int fill_cmd(FILE *disk, superblock_t *sb, char **args) {
  if (!disk || !sb) {
    printf("fill: internal error\n");
    return 1;
  }

  if (!args[1] || !args[2]) {
    printf("Usage: fill <count> <size> [prefix] [bs=<block>] "
           "[pattern=index|zero|random]\n");
    printf("Example: fill 10 1M test bs=64K pattern=random\n");
    return 1;
  }

  errno = 0;
  char *endcount = NULL;
  unsigned long long count_val = strtoull(args[1], &endcount, 10);
  if (errno != 0 || !endcount || *endcount != '\0' || count_val == 0 ||
      count_val > 1000000ULL) {
    printf("fill: invalid count '%s'\n", args[1]);
    return 1;
  }
  uint32_t count = (uint32_t)count_val;

  uint32_t size_bytes = 0;
  if (parse_size_arg(args[2], &size_bytes) != 0) {
    printf("fill: invalid size '%s'\n", args[2]);
    return 1;
  }

  const char *prefix = NULL;
  uint32_t block_bytes = FILL_DEFAULT_BLOCK;
  enum {
    FILL_PATTERN_INDEX,
    FILL_PATTERN_ZERO,
    FILL_PATTERN_RANDOM
  } pattern = FILL_PATTERN_INDEX;

  for (int idx = 3; args[idx]; idx++) {
    if (strncmp(args[idx], "bs=", 3) == 0) {
      uint32_t parsed = 0;
      if (parse_size_arg(args[idx] + 3, &parsed) != 0 || parsed == 0) {
        printf("fill: invalid block size '%s'\n", args[idx] + 3);
        return 1;
      }
      block_bytes = parsed;
    } else if (strncmp(args[idx], "pattern=", 8) == 0) {
      const char *opt = args[idx] + 8;
      if (strcasecmp(opt, "index") == 0) {
        pattern = FILL_PATTERN_INDEX;
      } else if (strcasecmp(opt, "zero") == 0) {
        pattern = FILL_PATTERN_ZERO;
      } else if (strcasecmp(opt, "random") == 0) {
        pattern = FILL_PATTERN_RANDOM;
      } else {
        printf("fill: invalid pattern '%s' (expected index|zero|random)\n",
               opt);
        return 1;
      }
    } else if (!prefix) {
      prefix = args[idx];
    } else {
      printf("fill: unexpected argument '%s'\n", args[idx]);
      return 1;
    }
  }

  if (!prefix)
    prefix = "fill";

  uint32_t chunk_bytes = block_bytes ? block_bytes : FILL_DEFAULT_BLOCK;
  if (chunk_bytes > size_bytes)
    chunk_bytes = size_bytes;
  if (chunk_bytes == 0)
    chunk_bytes = 1;

  uint8_t *buffer = malloc(chunk_bytes);
  if (!buffer) {
    printf("fill: unable to allocate buffer (%" PRIu32 " bytes)\n", chunk_bytes);
    return 1;
  }

  int random_seeded = 0;

  int width = 1;
  unsigned int tmp = count;
  while (tmp >= 10) {
    tmp /= 10;
    width++;
  }

  double start = monotonic_seconds();
  uint32_t created = 0;

  for (uint32_t i = 1; i <= count; i++) {
    char relative_name[512];
    if (snprintf(relative_name, sizeof(relative_name), "%s_%0*u.bin", prefix,
                 width, i) >= (int)sizeof(relative_name)) {
      printf("fill: resulting name too long for prefix '%s'\n", prefix);
      break;
    }

    char absolute_path[1024];
    if (resolve_path(relative_name, absolute_path, sizeof absolute_path) != 0) {
      printf("fill: invalid target path '%s'\n", relative_name);
      break;
    }

    inode_t existing;
    if (stat_bsfs(disk, sb, absolute_path, &existing) == 0) {
      (void)remove_file_bsfs(disk, sb, absolute_path);
    }

    if (create_file_bsfs(disk, sb, absolute_path) != 0) {
      printf("fill: cannot create '%s'\n", absolute_path);
      break;
    }

    file_t file;
    if (open_bsfs(disk, sb, absolute_path, &file) != 0) {
      printf("fill: cannot open '%s'\n", absolute_path);
      (void)remove_file_bsfs(disk, sb, absolute_path);
      break;
    }

    size_t remaining = size_bytes;

    while (remaining > 0) {
      size_t chunk = remaining < chunk_bytes ? remaining : chunk_bytes;

      switch (pattern) {
      case FILL_PATTERN_INDEX:
        memset(buffer, (uint8_t)(i & 0xFF), chunk);
        break;
      case FILL_PATTERN_ZERO:
        memset(buffer, 0, chunk);
        break;
      case FILL_PATTERN_RANDOM:
        if (!random_seeded) {
          srand((unsigned int)time(NULL));
          random_seeded = 1;
        }
        for (size_t j = 0; j < chunk; j++)
          buffer[j] = (uint8_t)(rand() & 0xFF);
        break;
      }

      size_t written = write_bsfs(disk, sb, &file, buffer, (uint32_t)chunk);
      if (written != chunk) {
        printf("fill: write error on '%s'\n", absolute_path);
        (void)remove_file_bsfs(disk, sb, absolute_path);
        remaining = 0;
        break;
      }
      remaining -= written;
    }

    close_bsfs(&file);

    if (remaining == 0) {
      created++;
    } else {
      break;
    }
  }

  double elapsed = monotonic_seconds() - start;
  free(buffer);

  const char *pattern_name =
      (pattern == FILL_PATTERN_INDEX)
          ? "index"
          : (pattern == FILL_PATTERN_ZERO ? "zero" : "random");

  double elapsed_ms = elapsed * 1000.0;
  printf("fill: created %u/%u files of %" PRIu32 " bytes (bs=%" PRIu32
         ", pattern=%s) in %.6f ms\n",
         created, count, size_bytes, block_bytes, pattern_name, elapsed_ms);

  return 1;
}
