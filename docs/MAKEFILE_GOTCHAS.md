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

**ASan + valgrind no conviven.** ASan reemplaza `malloc`, memcheck pierde
su instrumentación. Workflow: `make clean && make debug && make valgrind`.
