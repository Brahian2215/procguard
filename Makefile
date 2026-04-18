CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
          -Wcast-align -Wstrict-prototypes -Wmissing-prototypes \
          -D_GNU_SOURCE -pthread
CFLAGS_DEBUG := -g3 -O0 -DDEBUG
CFLAGS_RELEASE := -O2 -DNDEBUG
CFLAGS_ASAN := -g3 -O1 -fsanitize=address -fsanitize=undefined \
               -fno-omit-frame-pointer

LDFLAGS := -pthread
LDLIBS := -lncursesw

SRC_DIR := src
BUILD_DIR := build
TEST_DIR := tests

.PHONY: all debug release asan test clean format lint help

all: debug

debug: CFLAGS += $(CFLAGS_DEBUG)
debug: $(BUILD_DIR)/procguard

release: CFLAGS += $(CFLAGS_RELEASE)
release: $(BUILD_DIR)/procguard

asan: CFLAGS += $(CFLAGS_ASAN)
asan: LDFLAGS += -fsanitize=address -fsanitize=undefined
asan: $(BUILD_DIR)/procguard

test:
	@echo "Run 'make test' after tests exist"
	@exit 1

clean:
	rm -rf $(BUILD_DIR)

format:
	find $(SRC_DIR) $(TEST_DIR) -name '*.c' -o -name '*.h' | \
	  xargs clang-format -i

lint:
	find $(SRC_DIR) -name '*.c' | xargs -I{} clang-tidy {} -- $(CFLAGS)

help:
	@echo "Targets: debug, release, asan, test, clean, format, lint"

$(BUILD_DIR)/procguard:
	@echo "No source files yet. Create src/main.c first."
	@exit 1
