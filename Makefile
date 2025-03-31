CC = gcc
CFLAGS = -Wall -Wextra -Werror
BIN=bin
SRC_DIR = src
FMT_FILE = $(SRC_DIR)/format/formatter_bsfs.c

format:
	mkdir -p bin
	$(CC) $(CFLAGS) -o $(BIN)/formatter $(FMT_FILE)

clean:
	rm -rf bin

.PHONY: format clean
