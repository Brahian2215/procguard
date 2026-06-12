# Gotchas del compilador y Makefile

Flags estrictos activos: `-Wall -Wextra -Werror -Wshadow -Wpointer-arith
-Wcast-align -Wstrict-prototypes -Wmissing-prototypes`. Cada entrada:
síntoma → fix.

---

**`%*lu`/`%*ld`/`%*lld` rechazados por `-Werror=format=`.** No se puede
mezclar supresor `*` con length modifier. Usar `%*s` para saltar tokens
(consume hasta whitespace).

**`-Werror=format-truncation=` al concatenar paths con `snprintf`.** Si
`dir[N]` y `path[N]` son del mismo tamaño, GCC cree que `snprintf` puede
truncar. Fix: dimensionar `path` mayor que `dir + sufijo` (ej. `dir[128]`,
`path[160]`).

**`-Wmissing-prototypes` en helpers.** Todo helper interno del `.c` va
`static`. Sólo funciones públicas (declaradas en `.h`) sin `static`.

**`-Wshadow` con locales que ocultan parámetros.** Renombrar (`pid` →
`pid_str`). No silenciar con `__attribute__((unused))`.

**`-Wcast-align` al castear `char*` a puntero a struct.** Usar
`memcpy(&dest, buf, sizeof(dest))`, no cast directo.

**ASAN oculta assertions de Unity en RED.** Cuando un `TEST_ASSERT_*` falla
sale con `longjmp`, se salta el cleanup, y ASAN vuelca leaks que tapan el
mensaje. Fix: `make test-quick` (setea `ASAN_OPTIONS=detect_leaks=0`) para
iterar. `make test` mantiene leak detection para verificación final.

**Unity vendored falla con `-Werror`.** Tiene warnings propios. Compilar
`unity.o` con regla separada, flags mínimas (`-std=c11 -O1 -g3`), sin
heredar `$(CFLAGS)` del proyecto.

**inih vendored también falla con `-Werror`.** Mismo tratamiento que Unity:
regla `$(TESTS_BUILD_DIR)/ini.o` con flags mínimas + ASAN+UBSAN (se linkea
en tests de M4). Para la integración en el binario `procguard` (Fase 7
Slice 4), añadir una variante sin ASAN cuando main.c empiece a cargar el
INI.

**ASan + valgrind no conviven.** ASan reemplaza `malloc`, memcheck pierde
su instrumentación. Workflow: `make clean && make debug && make valgrind`.

**Sin dependencias de headers → objetos stale tras editar un `.h`.** Las
reglas `$(TESTS_BUILD_DIR)/X.o: src/.../X.c` listan solo el `.c`, no los
headers. Al cambiar un header compartido (p.ej. añadir un campo a
`pg_global_config_t` en `alert_config.h`, o al engine en `alert_internal.h`),
make NO recompila los `.o` que lo incluyen → quedan con el **layout viejo del
struct** mientras el binario de test se recompila con el nuevo → ABI mismatch:
campos leídos en offsets equivocados (síntoma: un test que valida `dry_run` u
otro campo falla de forma determinista y "imposible"). Fix: **`make clean`
después de editar cualquier header**, antes de `make test`/`test-quick`. (Fix
de fondo: `-MMD -MP` con `-include $(DEPS)`; pendiente, fuera de scope.)

**inih SÍ compila limpio bajo las flags estrictas del proyecto** (verificado
en Slice 4c). El binario `procguard` lo compila en el mismo `gcc` que el resto
de fuentes (no necesita objeto relajado aparte). La regla relajada
`$(TESTS_BUILD_DIR)/ini.o` se mantiene solo para los binarios de test (mezcla
con ASAN de otros `.o`).
