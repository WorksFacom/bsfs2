CC ?= cc

# Only targets that link the old userspace implementation need its source tree.
ifeq ($(strip $(OLD_BSFS_DIR)),)
ifneq ($(filter-out clean help bin/bench-posix,$(or $(MAKECMDGOALS),all)),)
$(error OLD_BSFS_DIR is required; pass OLD_BSFS_DIR=/absolute/path/to/bsfs_old)
endif
endif

CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wpedantic -MMD -MP
OLD_SOURCE_CFLAGS := -O2 -g -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -MMD -MP
LDLIBS += -pthread

BIN_DIR := bin
BUILD_DIR := build

COMMON_SOURCES := \
	src/bench_main.c \
	src/device_stats.c \
	src/pattern.c \
	src/result_jsonl.c \
	src/timing.c \
	src/workload.c \
	src/workload_bulk.c \
	src/workload_common.c \
	src/workload_concurrency.c \
	src/workload_metadata.c \
	src/workload_mixed.c \
	src/workload_old_recovery.c \
	src/workload_recovery.c

POSIX_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/posix/%.o,$(COMMON_SOURCES)) \
	$(BUILD_DIR)/posix/backend_posix.o

OLD_CPPFLAGS := $(CPPFLAGS) -I$(OLD_BSFS_DIR)/src/include
OLD_COMMON_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/old/%.o,$(COMMON_SOURCES)) \
	$(BUILD_DIR)/old/backend_old_bsfs.o
ifneq ($(strip $(OLD_BSFS_DIR)),)
OLD_OPERATION_SOURCES := $(shell find $(OLD_BSFS_DIR)/src/operations -type f -name '*.c' | sort)
endif
OLD_OPERATION_OBJECTS := $(patsubst $(OLD_BSFS_DIR)/%.c,$(BUILD_DIR)/oldlib/%.o,$(OLD_OPERATION_SOURCES))
OLD_API_OBJECT := $(BUILD_DIR)/oldlib/src/browser/api_bsfs.o
OLD_FORMAT_OBJECT := $(BUILD_DIR)/oldlib/src/format/mkfs_bsfs.o
DEPENDENCIES := $(POSIX_OBJECTS:.o=.d) $(OLD_COMMON_OBJECTS:.o=.d) \
	$(OLD_OPERATION_OBJECTS:.o=.d) $(OLD_API_OBJECT:.o=.d) \
	$(OLD_FORMAT_OBJECT:.o=.d)

.PHONY: all clean check old

all: $(BIN_DIR)/bench-posix old

$(BIN_DIR)/bench-posix: $(POSIX_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/posix/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

old: $(BIN_DIR)/bench-old $(BIN_DIR)/mkfs-old-bsfs

$(BIN_DIR)/bench-old: $(OLD_COMMON_OBJECTS) $(OLD_API_OBJECT) $(OLD_OPERATION_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/mkfs-old-bsfs: $(OLD_FORMAT_OBJECT) $(OLD_OPERATION_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/old/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(OLD_CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/oldlib/%.o: $(OLD_BSFS_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -I$(OLD_BSFS_DIR)/src/include $(OLD_SOURCE_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

check: all
	./tests/run.sh
	python3 -m unittest discover -s tests -p 'test_*.py'

-include $(DEPENDENCIES)
