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

.PHONY: all debug release asan test test-quick valgrind clean format lint lint-funclen help

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
	@echo "Build:  debug, release, asan"
	@echo "Test:   test (full + leak detection), test-quick (no leak detect, fast iteration)"
	@echo "Lint:   format, lint, lint-funclen (flag funciones >50 lineas)"
	@echo "Other:  valgrind, clean"

# lint-funclen: flaggea funciones de mas de 50 lineas (regla CLAUDE.md).
# Cuenta lineas entre la firma de funcion (paren abierto en col 1) y el }
# de cierre en col 1. Heuristica simple, suficiente para nuestro estilo.
FUNCLEN_MAX := 50
# Excluye codigo vendored (cjson, inih, unity) — no aplican nuestras reglas.
lint-funclen:
	@violations=0; \
	for f in $$(find $(SRC_DIR) -name '*.c' \
	             -not -path '*/cjson/*' \
	             -not -path '*/inih/*'); do \
		awk -v max=$(FUNCLEN_MAX) -v file="$$f" ' \
			/^[a-zA-Z_].*\(.*$$/ { name=$$0; start=NR; next } \
			/^}$$/ { if (name && (NR-start+1) > max) { \
				printf "%s:%d: %d lines: %s\n", file, start, NR-start+1, name; \
				violations++ } name="" } \
			END { exit (violations > 0 ? 1 : 0) }' "$$f" || violations=1; \
	done; \
	if [ $$violations -eq 0 ]; then echo "lint-funclen: OK (todas las funciones <= $(FUNCLEN_MAX) lineas)"; \
	else echo "lint-funclen: FAIL (refactoriza las funciones listadas)"; exit 1; fi

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

# test_metrics: M3 es función pura; metrics.c se compila inline.
$(BUILD_DIR)/test_metrics: $(TEST_UNIT_DIR)/test_metrics.c \
		src/metrics/metrics.c $(BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CFLAGS_TEST) $(CFLAGS_ASAN) \
	  -I$(UNITY_DIR) -Isrc/common -Isrc/metrics \
	  $^ -o $@ $(LDFLAGS) -fsanitize=address -fsanitize=undefined

# test_rank: comparador puro de ranked_t para qsort descendente.
$(BUILD_DIR)/test_rank: $(TEST_UNIT_DIR)/test_rank.c \
		src/util/rank.c $(BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CFLAGS_TEST) $(CFLAGS_ASAN) \
	  -I$(UNITY_DIR) -Isrc/common -Isrc/util \
	  $^ -o $@ $(LDFLAGS) -fsanitize=address -fsanitize=undefined

# test_store: M2 Sample Store. Linkea collector.c también para el test de
# integración end-to-end scan → insert → get_history.
$(BUILD_DIR)/test_store: $(TEST_UNIT_DIR)/test_store.c \
		src/store/store.c src/collector/collector.c $(BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CFLAGS_TEST) $(CFLAGS_ASAN) \
	  -I$(UNITY_DIR) -Isrc/common -Isrc/store -Isrc/collector \
	  $^ -o $@ $(LDFLAGS) -fsanitize=address -fsanitize=undefined

TEST_BINS := $(BUILD_DIR)/test_collector $(BUILD_DIR)/test_metrics \
             $(BUILD_DIR)/test_rank $(BUILD_DIR)/test_store

test: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t ==="; \
		$$t || failed=1; \
	done; \
	exit $$failed

# test-quick: itera rapido en RED-GREEN. Desactiva leak detection para que
# las assertions fallidas no queden ocultas por dump de leaks (los tests
# que abortan via TEST_ASSERT no llegan al cleanup, generando leaks falsos
# desde el punto de vista del ciclo TDD). Para verificacion final usar `test`.
test-quick: $(TEST_BINS)
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "=== $$t (quick, no leak detect) ==="; \
		ASAN_OPTIONS=detect_leaks=0 $$t || failed=1; \
	done; \
	exit $$failed

# Nota Sesión 1: el stub src/main.c retorna 1 ("not yet implemented"),
# así que `make valgrind` saldrá con exit 1 por el programa, no por leaks.
# Se vuelve verde en Sesión 3 cuando main.c sea real.
valgrind: $(BUILD_DIR)/procguard
	valgrind --leak-check=full --error-exitcode=1 $(BUILD_DIR)/procguard

$(BUILD_DIR)/procguard: src/collector/collector.c src/metrics/metrics.c \
		src/util/rank.c src/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Isrc/common -Isrc/collector -Isrc/metrics -Isrc/util \
	  $^ -o $@ $(LDFLAGS) $(LDLIBS)
