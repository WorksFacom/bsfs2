CC = gcc
CFLAGS = -Wall -Wextra -Werror -Isrc/include
BIN = bin
SRC_DIR = src

# Source files
FMT_FILE = $(SRC_DIR)/format/formatter_bsfs.c
BROWSER_FILE = $(SRC_DIR)/browser/browser_bsfs.c
COMMANDS_FILE = $(SRC_DIR)/browser/commands_bsfs.c

# Command files
COMMAND_FILES = $(SRC_DIR)/browser/commands/cd.c \
                $(SRC_DIR)/browser/commands/pwd.c \
                $(SRC_DIR)/browser/commands/list.c \
                $(SRC_DIR)/browser/commands/mkdir.c \
                $(SRC_DIR)/browser/commands/rmdir.c \
                $(SRC_DIR)/browser/commands/touch.c \
                $(SRC_DIR)/browser/commands/rm.c \
                $(SRC_DIR)/browser/commands/cp.c \
                $(SRC_DIR)/browser/commands/mv.c \
                $(SRC_DIR)/browser/commands/cat.c \
                $(SRC_DIR)/browser/commands/echo.c

# Operation files (add as needed based on your implementations)
OPERATION_FILES = $(SRC_DIR)/operations/struct_operations/allocate_bsfs.c \
                  $(SRC_DIR)/operations/struct_operations/bitmap_operations_bsfs.c \
                  $(SRC_DIR)/operations/struct_operations/block_operations_bsfs.c \
                  $(SRC_DIR)/operations/struct_operations/inode_operations_bsfs.c \
                  $(SRC_DIR)/operations/struct_operations/superblock_operations_bsfs.c \
                  $(SRC_DIR)/operations/directory_operations/directory_operations_bsfs.c \
                  $(SRC_DIR)/operations/directory_operations/dir_entry_operations_bsfs.c \
                  $(SRC_DIR)/operations/directory_operations/path_utils_bsfs.c \
                  $(SRC_DIR)/operations/file_operations/file_operations.c \
                  $(SRC_DIR)/operations/data_structs/bspan_queue_bsfs.c \
                  $(SRC_DIR)/operations/data_structs/btree_bsfs.c

# All browser source files
BROWSER_SOURCES = $(BROWSER_FILE) $(COMMANDS_FILE) $(COMMAND_FILES) $(OPERATION_FILES)

all: format browser

format:
	mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $(BIN)/formatter $(FMT_FILE) $(OPERATION_FILES)

browser:
	mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $(BIN)/browser $(BROWSER_SOURCES)

clean:
	rm -rf $(BIN)

.PHONY: all format browser clean 
