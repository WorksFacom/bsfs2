CC ?= cc

CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wpedantic -MMD -MP
LDLIBS += -pthread

BIN_DIR := bin
BUILD_DIR := build

SOURCES := \
	src/backend_posix.c \
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
	src/workload_recovery.c

OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)

.PHONY: all clean check

all: $(BIN_DIR)/bench-posix

$(BIN_DIR)/bench-posix: $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

check: all
	./tests/run.sh
	python3 -m unittest discover -s tests -p 'test_*.py'

-include $(DEPENDENCIES)
