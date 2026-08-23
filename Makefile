CC       = clang
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  =

SRC_DIR   = src
TEST_DIR  = tests
BUILD_DIR = build

CORE_SRCS = $(SRC_DIR)/filesystem.c $(SRC_DIR)/process.c $(SRC_DIR)/scheduler.c $(SRC_DIR)/shell.c
CORE_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

TARGET      = minios
TEST_TARGET = run_tests

.PHONY: all debug test asan clean

all: CFLAGS += -O2
all: $(TARGET)

debug: CFLAGS += -g -O0 -DDEBUG
debug: $(TARGET)

asan: CFLAGS += -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TARGET)

$(TARGET): $(BUILD_DIR)/main.o $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# --- tests -----------------------------------------------------------
# Each test file is compiled together with the core sources (minus
# main.c) into its own standalone binary.

TEST_SRCS = $(wildcard $(TEST_DIR)/test_*.c)
TEST_BINS = $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%,$(TEST_SRCS))

test: CFLAGS += -g -O0
test: $(TEST_BINS)
	@echo "==== Running tests ===="
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "--- $$t ---"; \
		./$$t || status=1; \
	done; \
	exit $$status

$(BUILD_DIR)/test_%: $(TEST_DIR)/test_%.c $(CORE_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_SRCS) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

