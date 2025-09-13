/**
 * @file su.c
 * @brief Implements su command to switch session identity (testing).
 */
#include <stdio.h>
#include <stdlib.h>
#include "api_bsfs.h"

int su_cmd(FILE *disk, superblock_t *sb, char **args) {
  (void)disk; (void)sb;
  if (!args[1]) {
    printf("Usage: su <uid> [gid]\n");
    return 1;
  }
  char *e1=NULL, *e2=NULL;
  unsigned long uid = strtoul(args[1], &e1, 10);
  if (e1==args[1] || *e1!='\0') { printf("su: invalid uid\n"); return 1; }
  unsigned long gid = uid;
  if (args[2]) {
    gid = strtoul(args[2], &e2, 10);
    if (e2==args[2] || *e2!='\0') { printf("su: invalid gid\n"); return 1; }
  }
  set_identity_bsfs((uint16_t)uid, (uint16_t)gid);
  printf("identity: uid=%lu gid=%lu\n", uid, gid);
  return 1;
}

