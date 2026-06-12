CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Wshadow -Wpointer-arith \
          -Wcast-align -Wstrict-prototypes -Wmissing-prototypes \
          -D_GNU_SOURCE -pthread
CFLAGS_DEBUG := -g3 -O0 -DDEBUG
CFLAGS_RELEASE := -O2 -DNDEBUG
CFLAGS_ASAN := -g3 -O1 -fsanitize=address -fsanitize=undefined \
               -fno-omit-frame-pointer
# TSan: detecta data races; ASan NO lo hace. Mutuamente excluyente con ASan
# (igual que valgrind). Hoy procguard es monohilo, así que este target solo
# prueba que las flags compilan limpio; los tests con hilos reales (stress de
# colas con pthread_create) llegan en Slice 5 y se ejecutan bajo este target.
CFLAGS_TSAN := -g3 -O1 -fsanitize=thread -fno-omit-frame-pointer
CFLAGS_TEST := -g3 -O1

LDFLAGS := -pthread
LDLIBS := -lncursesw

SRC_DIR        := src
BUILD_DIR      := build
TESTS_BUILD_DIR := $(BUILD_DIR)/tests
TEST_DIR       := tests
UNITY_DIR      := tests/unity
TEST_UNIT_DIR  := tests/unit

# Flags unificadas para compilar objetos y binarios de test con ASAN+UBSAN.
# Todo module source compilado bajo test lleva estas flags; los include dirs
# cubren los headers cruzados que cualquier test podría consumir.
TEST_INCLUDES := -I$(UNITY_DIR) -Isrc/common \
                 -Isrc/collector -Isrc/metrics -Isrc/store -Isrc/ipc \
                 -Isrc/alert -Isrc/common/inih
TEST_CFLAGS   := $(CFLAGS) $(CFLAGS_TEST) $(CFLAGS_ASAN) $(TEST_INCLUDES)
TEST_LDFLAGS  := $(LDFLAGS) -fsanitize=address -fsanitize=undefined

.PHONY: all debug release asan tsan test test-quick valgrind clean format lint lint-funclen help

all: debug

debug: CFLAGS += $(CFLAGS_DEBUG)
debug: $(BUILD_DIR)/procguard

release: CFLAGS += $(CFLAGS_RELEASE)
release: $(BUILD_DIR)/procguard

asan: CFLAGS += $(CFLAGS_ASAN)
asan: LDFLAGS += -fsanitize=address -fsanitize=undefined
asan: $(BUILD_DIR)/procguard

# tsan: build del binario bajo ThreadSanitizer. Scaffold para Slice 5 — cuando
# existan los tres hilos reales, las variantes de test con hilos correrán aquí.
# Workflow (TSan y ASan no conviven): make clean && make tsan
tsan: CFLAGS += $(CFLAGS_TSAN)
tsan: LDFLAGS += -fsanitize=thread
tsan: $(BUILD_DIR)/procguard

clean:
	rm -rf $(BUILD_DIR)

format:
	find $(SRC_DIR) $(TEST_DIR) -name '*.c' -o -name '*.h' | \
	  xargs clang-format -i

lint:
	find $(SRC_DIR) -name '*.c' | xargs -I{} clang-tidy {} -- $(CFLAGS)

help:
	@echo "Build:  debug, release, asan, tsan (ThreadSanitizer, Slice 5+)"
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

$(TESTS_BUILD_DIR): | $(BUILD_DIR)
	mkdir -p $@

# Unity: vendored third-party — sin -Werror ni $(CFLAGS) del proyecto.
$(TESTS_BUILD_DIR)/unity.o: $(UNITY_DIR)/unity.c | $(TESTS_BUILD_DIR)
	$(CC) -std=c11 -O1 -g3 -I$(UNITY_DIR) -c $< -o $@

# inih: vendored third-party — mismo patrón que Unity (sin -Werror ni flags
# estrictos del proyecto). Se linkea en tests de M4 desde Fase 2; la
# integración en el binario procguard (sin ASAN) llega con Fase 7.
$(TESTS_BUILD_DIR)/ini.o: src/common/inih/ini.c src/common/inih/ini.h | $(TESTS_BUILD_DIR)
	$(CC) -std=c11 -O1 -g3 -fsanitize=address -fsanitize=undefined \
	  -fno-omit-frame-pointer -c $< -o $@

# Objetos de módulos del proyecto compilados bajo ASAN+UBSAN para reutilizar
# entre binarios de test. Evita recompilar los mismos .c en cada test_X.
$(TESTS_BUILD_DIR)/collector.o: src/collector/collector.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/metrics.o: src/metrics/metrics.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/store.o: src/store/store.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/queue.o: src/ipc/queue.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert_policy.o: src/alert/alert_policy.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert_state.o: src/alert/alert_state.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert.o: src/alert/alert.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert_eval.o: src/alert/alert_eval.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert_validate.o: src/alert/alert_validate.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert_act.o: src/alert/alert_act.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(TESTS_BUILD_DIR)/alert_cage.o: src/alert/alert_cage.c | $(TESTS_BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_collector: $(TEST_UNIT_DIR)/test_collector.c \
		$(TESTS_BUILD_DIR)/collector.o $(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

$(BUILD_DIR)/test_metrics: $(TEST_UNIT_DIR)/test_metrics.c \
		$(TESTS_BUILD_DIR)/metrics.o $(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

# test_store linkea collector.o para el test end-to-end scan→insert→history.
$(BUILD_DIR)/test_store: $(TEST_UNIT_DIR)/test_store.c \
		$(TESTS_BUILD_DIR)/store.o $(TESTS_BUILD_DIR)/collector.o \
		$(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

$(BUILD_DIR)/test_queues: $(TEST_UNIT_DIR)/test_queues.c \
		$(TESTS_BUILD_DIR)/queue.o $(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

# test_alert_parser linkea alert_policy.o (lógica bajo prueba) + ini.o (vendor
# inih, callback de parseo) + unity.o. Primer test de Slice 4a Fase 2.
$(BUILD_DIR)/test_alert_parser: $(TEST_UNIT_DIR)/test_alert_parser.c \
		$(TESTS_BUILD_DIR)/alert_policy.o $(TESTS_BUILD_DIR)/ini.o \
		$(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

# test_alert_state linkea store.o porque los tests de gc construyen un
# pg_store_t real para validar el chequeo de presencia (Slice 4b Fase 3).
$(BUILD_DIR)/test_alert_state: $(TEST_UNIT_DIR)/test_alert_state.c \
		$(TESTS_BUILD_DIR)/alert_state.o $(TESTS_BUILD_DIR)/store.o \
		$(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

# test_alert_engine: engine init/destroy + state machine evaluate (Slice 4b
# Fase 4) + validate (Fase 5b) + act (Fase 6). Linkea alert + alert_eval +
# alert_validate + alert_act + alert_policy (catalog) + alert_state (registry)
# + store/metrics (deltas) + collector (read_starttime, guard TOCTOU) + ini +
# unity.
$(BUILD_DIR)/test_alert_engine: $(TEST_UNIT_DIR)/test_alert_engine.c \
		$(TESTS_BUILD_DIR)/alert.o $(TESTS_BUILD_DIR)/alert_eval.o \
		$(TESTS_BUILD_DIR)/alert_validate.o $(TESTS_BUILD_DIR)/alert_act.o \
		$(TESTS_BUILD_DIR)/alert_cage.o \
		$(TESTS_BUILD_DIR)/alert_policy.o $(TESTS_BUILD_DIR)/alert_state.o \
		$(TESTS_BUILD_DIR)/store.o $(TESTS_BUILD_DIR)/metrics.o \
		$(TESTS_BUILD_DIR)/collector.o \
		$(TESTS_BUILD_DIR)/ini.o $(TESTS_BUILD_DIR)/unity.o | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) $^ -o $@ $(TEST_LDFLAGS)

TEST_BINS := $(BUILD_DIR)/test_collector $(BUILD_DIR)/test_metrics \
             $(BUILD_DIR)/test_store $(BUILD_DIR)/test_queues \
             $(BUILD_DIR)/test_alert_parser $(BUILD_DIR)/test_alert_state \
             $(BUILD_DIR)/test_alert_engine

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

valgrind: $(BUILD_DIR)/procguard
	valgrind --leak-check=full --error-exitcode=1 $(BUILD_DIR)/procguard

# procguard: orquestador de gobernanza (Slice 4b Fase 7). Linkea M1/M2/M3 +
# el engine M4 completo (alert + eval + validate + act + policy + state) +
# inih (vendored; compila limpio bajo las flags estrictas, verificado). Un solo
# gcc: las fuentes del proyecto y inih comparten las flags del target activo
# (debug/release/asan/tsan), así los objetos casan al linkear bajo sanitizers.
$(BUILD_DIR)/procguard: src/collector/collector.c src/metrics/metrics.c \
		src/store/store.c \
		src/alert/alert.c src/alert/alert_eval.c src/alert/alert_validate.c \
		src/alert/alert_act.c src/alert/alert_cage.c \
		src/alert/alert_policy.c src/alert/alert_state.c \
		src/common/inih/ini.c src/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Isrc/common -Isrc/collector -Isrc/metrics -Isrc/store \
	  -Isrc/alert -Isrc/common/inih \
	  $^ -o $@ $(LDFLAGS) $(LDLIBS)
