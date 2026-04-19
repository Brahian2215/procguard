CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
          -Wcast-align -Wstrict-prototypes -Wmissing-prototypes \
          -D_GNU_SOURCE -pthread
CFLAGS_DEBUG := -g3 -O0 -DDEBUG
CFLAGS_RELEASE := -O2 -DNDEBUG
CFLAGS_ASAN := -g3 -O1 -fsanitize=address -fsanitize=undefined \
               -fno-omit-frame-pointer
CFLAGS_TEST := -g3 -O1

LDFLAGS := -pthread
LDLIBS := -lncursesw

SRC_DIR       := src
BUILD_DIR     := build
TEST_DIR      := tests
UNITY_DIR     := tests/unity
TEST_UNIT_DIR := tests/unit

.PHONY: all debug release asan test valgrind clean format lint help

all: debug

debug: CFLAGS += $(CFLAGS_DEBUG)
debug: $(BUILD_DIR)/procguard

release: CFLAGS += $(CFLAGS_RELEASE)
release: $(BUILD_DIR)/procguard

asan: CFLAGS += $(CFLAGS_ASAN)
asan: LDFLAGS += -fsanitize=address -fsanitize=undefined
asan: $(BUILD_DIR)/procguard

clean:
	rm -rf $(BUILD_DIR)

format:
	find $(SRC_DIR) $(TEST_DIR) -name '*.c' -o -name '*.h' | \
	  xargs clang-format -i

lint:
	find $(SRC_DIR) -name '*.c' | xargs -I{} clang-tidy {} -- $(CFLAGS)

help:
	@echo "Targets: debug, release, asan, test, valgrind, clean, format, lint"

$(BUILD_DIR):
	mkdir -p $@

# Unity: vendored third-party — sin -Werror ni $(CFLAGS) del proyecto.
$(BUILD_DIR)/unity.o: $(UNITY_DIR)/unity.c | $(BUILD_DIR)
	$(CC) -std=c11 -O1 -g3 -I$(UNITY_DIR) -c $< -o $@

# test_collector: fuentes inline con ASAN+UBSAN.
# Compila collector.c en el binario de test (evita conflicto con build normal).
$(BUILD_DIR)/test_collector: $(TEST_UNIT_DIR)/test_collector.c \
		src/collector/collector.c $(BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CFLAGS_TEST) $(CFLAGS_ASAN) \
	  -I$(UNITY_DIR) -Isrc/common -Isrc/collector \
	  $^ -o $@ $(LDFLAGS) -fsanitize=address -fsanitize=undefined

TEST_BINS := $(BUILD_DIR)/test_collector

test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		$$t || failed=1; \
	done; \
	exit $$failed

# Nota Sesión 1: el stub src/main.c retorna 1 ("not yet implemented"),
# así que `make valgrind` saldrá con exit 1 por el programa, no por leaks.
# Se vuelve verde en Sesión 3 cuando main.c sea real.
valgrind: $(BUILD_DIR)/procguard
	valgrind --leak-check=full --error-exitcode=1 $(BUILD_DIR)/procguard

# Stub binario: desbloquea make debug/asan hasta que main.c sea real.
$(BUILD_DIR)/procguard: src/collector/collector.c src/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Isrc/common -Isrc/collector \
	  $^ -o $@ $(LDFLAGS) $(LDLIBS)
