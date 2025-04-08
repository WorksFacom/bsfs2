CC = gcc
CFLAGS = -Wall -Wextra -Werror
BIN=bin
SRC_DIR = src
FMT_FILE = $(SRC_DIR)/format/formatter_bsfs.c
BROWSER_FILE = $(SRC_DIR)/browser/browser_bsfs.c
COMMANDS_FILE = $(SRC_DIR)/browser/commands_bsfs.c

format:
	mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN)/formatter $(FMT_FILE)

clean:
	rm -rf bin

browser:
	mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN)/browser $(COMMANDS_FILE) $(BROWSER_FILE)

.PHONY: format browser clean 
