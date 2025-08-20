/**
 * @file dd.c
 * @brief Implementation of dd command for BSFS
 * 
 * Creates files of specified size to test filesystem capacity and functionality.
 * Usage: dd if=<input> of=<output> bs=<block_size> count=<blocks>
 * Special inputs: /dev/zero (fills with zeros), /dev/random (random data)
 */

#include "../../include/commands_bsfs.h"
#include "../../include/api_bsfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_BLOCK_SIZE 512
#define MAX_BUFFER_SIZE (64 * 1024)

extern char current_path[1024];

/**
 * @brief Parse dd command arguments
 * @param args Command arguments
 * @param input_file Input file path (output parameter)
 * @param output_file Output file path (output parameter)
 * @param block_size Block size (output parameter)
 * @param count Number of blocks (output parameter)
 * @return 0 on success, -1 on error
 */
static int parse_dd_args(char **args, char **input_file, char **output_file, 
                        size_t *block_size, size_t *count) {
    *input_file = NULL;
    *output_file = NULL;
    *block_size = DEFAULT_BLOCK_SIZE;
    *count = 0;
    
    for (int i = 1; args[i] != NULL; i++) {
        if (strncmp(args[i], "if=", 3) == 0) {
            *input_file = args[i] + 3;
        } else if (strncmp(args[i], "of=", 3) == 0) {
            *output_file = args[i] + 3;
        } else if (strncmp(args[i], "bs=", 3) == 0) {
            char *endptr;
            long bs = strtol(args[i] + 3, &endptr, 10);
            
            if (*endptr == 'K' || *endptr == 'k') {
                bs *= 1024;
            } else if (*endptr == 'M' || *endptr == 'm') {
                bs *= 1024 * 1024;
            } else if (*endptr == 'G' || *endptr == 'g') {
                bs *= 1024 * 1024 * 1024;
            }
            
            if (bs <= 0 || bs > MAX_BUFFER_SIZE) {
                printf("dd: invalid block size '%s'\n", args[i] + 3);
                return -1;
            }
            *block_size = (size_t)bs;
        } else if (strncmp(args[i], "count=", 6) == 0) {
            char *endptr;
            long cnt = strtol(args[i] + 6, &endptr, 10);
            if (cnt <= 0) {
                printf("dd: invalid count '%s'\n", args[i] + 6);
                return -1;
            }
            *count = (size_t)cnt;
        }
    }
    
    if (*output_file == NULL) {
        printf("dd: missing output file (of=filename)\n");
        return -1;
    }
    
    if (*count == 0) {
        printf("dd: missing count (count=number)\n");
        return -1;
    }
    
    return 0;
}

/**
 * @brief Simple path resolution for relative paths
 * @param path Input path (may be relative)
 * @param absolute_path Output buffer for absolute path
 * @return 0 on success, -1 on error
 */
static int simple_resolve_path(const char *path, char *absolute_path) {
    if (path[0] == '/') {
        strcpy(absolute_path, path);
    } else {
        if (strcmp(current_path, "/") == 0) {
            snprintf(absolute_path, 1024, "/%s", path);
        } else {
            snprintf(absolute_path, 1024, "%s/%s", current_path, path);
        }
    }
    return 0;
}

/**
 * @brief Fill buffer with data based on input source
 * @param input_file Input source (/dev/zero, /dev/random, or file path)
 * @param buffer Buffer to fill
 * @param size Size to fill
 * @return Number of bytes filled, or -1 on error
 */
static ssize_t fill_buffer(const char *input_file, void *buffer, size_t size) {
    if (strcmp(input_file, "/dev/zero") == 0) {
        memset(buffer, 0, size);
        return size;
    } else if (strcmp(input_file, "/dev/random") == 0) {
        static int seeded = 0;
        if (!seeded) {
            srand((unsigned int)time(NULL));
            seeded = 1;
        }
        
        unsigned char *buf = (unsigned char *)buffer;
        for (size_t i = 0; i < size; i++) {
            buf[i] = (unsigned char)(rand() % 256);
        }
        return size;
    } else {
        printf("dd: reading from files not yet implemented\n");
        return -1;
    }
}

/**
 * @brief Implementation of dd command
 * @param disk File pointer to the disk
 * @param sb Superblock structure
 * @param args Command arguments
 * @return 0 on success, -1 on error
 */
int dd(FILE *disk, superblock_t *sb, char **args) {
    char *input_file, *output_file;
    size_t block_size, count;
    
    if (parse_dd_args(args, &input_file, &output_file, &block_size, &count) != 0) {
        printf("Usage: dd if=<input> of=<output> bs=<block_size> count=<blocks>\n");
        printf("       input can be /dev/zero or /dev/random\n");
        printf("       block_size can have K, M, G suffixes\n");
        printf("Example: dd if=/dev/zero of=testfile bs=1M count=10\n");
        return -1;
    }
    
    char absolute_path[1024];
    if (simple_resolve_path(output_file, absolute_path) != 0) {
        printf("dd: invalid output path '%s'\n", output_file);
        return -1;
    }
    
    inode_t inode;
    if (stat_bsfs(disk, sb, absolute_path, &inode) == 0) {
        printf("dd: file '%s' already exists\n", output_file);
        return -1;
    }
    
    if (create_file_bsfs(disk, sb, absolute_path) != 0) {
        printf("dd: cannot create file '%s'\n", output_file);
        return -1;
    }
    
    file_t file;
    if (open_bsfs(disk, sb, absolute_path, &file) != 0) {
        printf("dd: cannot open file '%s'\n", output_file);
        remove_file_bsfs(disk, sb, absolute_path);
        return -1;
    }
    
    void *buffer = malloc(block_size);
    if (buffer == NULL) {
        printf("dd: cannot allocate buffer of size %zu\n", block_size);
        close_bsfs(&file);
        remove_file_bsfs(disk, sb, absolute_path);
        return -1;
    }
    
    size_t total_bytes = 0;
    size_t blocks_written = 0;
    
    printf("dd: copying %zu blocks of %zu bytes each...\n", count, block_size);
    
    for (size_t i = 0; i < count; i++) {
        ssize_t bytes_read = fill_buffer(input_file, buffer, block_size);
        if (bytes_read <= 0) {
            printf("dd: error reading from input source\n");
            break;
        }
        
        size_t bytes_written = write_bsfs(disk, sb, &file, buffer, (uint32_t)bytes_read);
        if (bytes_written == 0) {
            printf("dd: write failed - filesystem may be full\n");
            break;
        }
        
        total_bytes += bytes_written;
        blocks_written++;
        
        if (count > 100 && (i + 1) % (count / 10) == 0) {
            printf("dd: %zu%% complete (%zu/%zu blocks)\n", 
                   ((i + 1) * 100) / count, i + 1, count);
        }
        
        if (bytes_written < bytes_read) {
            printf("dd: partial write - filesystem may be full\n");
            break;
        }
    }
    
    free(buffer);
    close_bsfs(&file);
    
    printf("dd: %zu blocks (%zu bytes) copied\n", blocks_written, total_bytes);
    
    if (blocks_written < count) {
        printf("dd: warning - only %zu of %zu blocks were written\n", 
               blocks_written, count);
        return -1;
    }
    
    return 0;
}