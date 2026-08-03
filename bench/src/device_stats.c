#define _GNU_SOURCE
#include "bench_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

int bench_device_sectors(const char *device, uint64_t *read_sectors,
                         uint64_t *written_sectors) {
  struct stat status;
  char path[PATH_MAX];
  FILE *input;
  unsigned long long fields[11] = {0};
  int count;

  if (!device || !read_sectors || !written_sectors)
    return -EINVAL;
  if (stat(device, &status) != 0)
    return -errno;
  if (!S_ISBLK(status.st_mode))
    return -ENOTBLK;
  if (snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/stat",
               major(status.st_rdev), minor(status.st_rdev)) >=
      (int)sizeof(path))
    return -ENAMETOOLONG;
  input = fopen(path, "r");
  if (!input)
    return -errno;
  count = fscanf(input,
                 "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                 &fields[0], &fields[1], &fields[2], &fields[3], &fields[4],
                 &fields[5], &fields[6], &fields[7], &fields[8], &fields[9],
                 &fields[10]);
  fclose(input);
  if (count < 7)
    return -EIO;
  *read_sectors = fields[2];
  *written_sectors = fields[6];
  return 0;
}
