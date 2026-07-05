#include "layout.h"

#include <stdio.h>
#include <stdlib.h>

long bitmap_size_blocks_from_disk_gib(long disk_size_gib, long page_size_kib) {
    if (disk_size_gib <= 0) {
        fprintf(stderr, "disk_size_gib must be > 0\n");
        exit(1);
    }
    if (page_size_kib <= 0) {
        fprintf(stderr, "page_size_kib must be > 0\n");
        exit(1);
    }
    return (disk_size_gib * KIB_PER_GIB) / page_size_kib;
}
