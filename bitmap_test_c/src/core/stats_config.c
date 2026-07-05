#include "stats_config.h"

#include <string.h>

#include "layout.h"

stat_config stat_config_default(void) {
    stat_config c;
    c.num_files = 100;
    c.file_size_mib = 10;
    c.block_size_kib = 4;
    c.disk_size_gib = DISK_SIZE_GIB;
    c.bitmap_size_override = 0;
    c.num_seeds = 1000;
    c.seed_base = 42;
    c.mode = "pareto";
    c.num_operations = 0;
    c.output_dir = ".";
    c.workload_label = "default";
    return c;
}

long stat_config_blocks_per_file(const stat_config *c) {
    return (c->file_size_mib * 1024) / c->block_size_kib;
}

long stat_config_target_blocks(const stat_config *c) {
    return c->num_files * stat_config_blocks_per_file(c);
}

long stat_config_bitmap_size(const stat_config *c) {
    if (c->bitmap_size_override > 0) {
        return c->bitmap_size_override;
    }
    return bitmap_size_blocks_from_disk_gib(c->disk_size_gib, PAGE_SIZE_KIB);
}

long stat_config_resolved_num_operations(const stat_config *c) {
    if (c->num_operations > 0) {
        return c->num_operations;
    }
    if (strcmp(c->mode, "allocation_only") == 0) {
        return c->num_files;
    }
    return 100000;
}

const char *stat_config_validate(const stat_config *c) {
    if (c->num_files <= 0) {
        return "num_files must be > 0";
    }
    if (c->file_size_mib <= 0) {
        return "file_size_mib must be > 0";
    }
    if (c->block_size_kib <= 0) {
        return "block_size_kib must be > 0";
    }
    if (c->disk_size_gib <= 0) {
        return "disk_size_gib must be > 0";
    }
    if (c->bitmap_size_override < 0 ||
        (c->bitmap_size_override != 0 && c->bitmap_size_override <= 0)) {
        return "bitmap_size_override must be > 0";
    }
    if (stat_config_target_blocks(c) > stat_config_bitmap_size(c)) {
        return "target workload exceeds bitmap capacity";
    }
    if (c->num_seeds <= 0) {
        return "num_seeds must be > 0";
    }
    if (stat_config_resolved_num_operations(c) <= 0) {
        return "num_operations must be > 0";
    }
    if (strcmp(c->mode, "allocation_only") != 0 &&
        strcmp(c->mode, "pareto") != 0 &&
        strcmp(c->mode, "multisize_pareto") != 0) {
        return "mode must be 'allocation_only', 'pareto', or 'multisize_pareto'";
    }
    return NULL;
}
