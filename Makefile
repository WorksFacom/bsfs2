CC = gcc
CFLAGS = -Wall -Wextra -Werror -Isrc/include
BIN = bin
SRC_DIR = src

# Source globs
BROWSER_FILE = $(SRC_DIR)/browser/browser_bsfs.c
COMMANDS_FILE = $(SRC_DIR)/browser/commands_bsfs.c
COMMAND_FILES = $(wildcard $(SRC_DIR)/browser/commands/*.c)
BROWSER_API = $(SRC_DIR)/browser/api_bsfs.c

# Auto-collect operation files recursively
OPERATION_FILES = $(shell find $(SRC_DIR)/operations -type f -name '*.c')

# Auto-collect all format sources
FORMAT_SRCS = $(wildcard $(SRC_DIR)/format/*.c)
FORMAT_BINS = $(patsubst $(SRC_DIR)/format/%.c,$(BIN)/%,$(FORMAT_SRCS))

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
                $(SRC_DIR)/browser/commands/chmod.c \
                $(SRC_DIR)/browser/commands/chown.c \
                $(SRC_DIR)/browser/commands/chgrp.c \
                $(SRC_DIR)/browser/commands/echo.c \
                $(SRC_DIR)/browser/commands/dd.c \
                $(SRC_DIR)/browser/commands/stat.c \
                $(SRC_DIR)/browser/commands/recovery.c \
                $(SRC_DIR)/browser/commands/restore.c \
                $(SRC_DIR)/browser/commands/purge.c \
                $(SRC_DIR)/browser/commands/whoami.c \
                $(SRC_DIR)/browser/commands/su.c \
                $(SRC_DIR)/browser/commands/umask.c

# All browser source files
BROWSER_SOURCES = $(BROWSER_FILE) $(COMMANDS_FILE) $(COMMAND_FILES) $(BROWSER_API) $(OPERATION_FILES)

all: format browser

format: $(FORMAT_BINS)
	@# Provide canonical binary name for mkfs
	@if [ -f $(BIN)/mkfs_bsfs ]; then cp $(BIN)/mkfs_bsfs $(BIN)/mkfs.bsfs; fi

# Pattern rule to build each formatter in src/format into bin/<name>
$(BIN)/%: $(SRC_DIR)/format/%.c $(OPERATION_FILES)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(OPERATION_FILES)

browser:
	mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $(BIN)/browser $(BROWSER_SOURCES)

clean:
	rm -rf $(BIN)

.PHONY: all format browser clean 
