# Makefile y compilador — gotchas conocidos

Warnings y errores no obvios de los flags estrictos de ProcGuard
(`-Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-align
-Wstrict-prototypes -Wmissing-prototypes`).

Cada entrada: síntoma → causa → cómo arreglarlo. Crece con la experiencia.

---

## `-Werror=format=` rechaza `%*lu`, `%*ld`, `%*lld` en `sscanf`/`printf`

**Síntoma**:
```
error: use of assignment suppression and length modifier together in
gnu_scanf format [-Werror=format=]
```

**Causa**: GCC con `-Wformat` no permite combinar el supresor `*` con un
length modifier (`l`, `ll`, `h`). Aplica en `sscanf`/`printf` family.

**Fix**: usar `%*s` para descartar tokens (consume hasta whitespace).
Funcional para campos de `/proc/[pid]/stat` que sólo se quieren saltar:

```c
/* MAL */
sscanf(line, " %c %ld %*lu %*lu %llu", &state, &ppid, &utime);

/* BIEN */
sscanf(line, " %c %ld %*s %*s %llu", &state, &ppid, &utime);
```

Visto en: `src/collector/collector.c::parse_stat_fields` (Slice 1 / Sesión 1).

---

## `-Werror=format-truncation=` con `snprintf` de paths

**Síntoma**:
```
error: '/stat' directive output may be truncated writing 5 bytes into
a region of size between 1 and 256 [-Werror=format-truncation=]
```

**Causa**: GCC analiza el rango posible del output de `snprintf`. Si el
formato + el contenido máximo de los argumentos puede exceder el buffer,
warninea aunque `snprintf` truncaría sin overflow.

**Fix**: dimensionar el buffer destino mayor que el de origen + sufijo
fijo. Para concatenar `dir + "/stat"`:

```c
/* MAL */
char dir[256];
char path[256];  /* mismo tamaño → puede truncarse */
snprintf(path, sizeof(path), "%s/stat", dir);

/* BIEN */
char dir[128];
char path[160];  /* dir(128) + "/stat"(5) + null(1) + holgura */
snprintf(path, sizeof(path), "%s/stat", dir);
```

Visto en: `tests/unit/test_collector.c::write_stat` (Slice 1 / Sesión 1).

---

## `-Wmissing-prototypes` y funciones helper

**Síntoma**:
```
error: no previous prototype for 'helper_fn' [-Werror=missing-prototypes]
```

**Causa**: el flag exige que toda función no-static tenga prototipo en
un header.

**Fix**: marcar helpers internos del `.c` como `static`. Sólo las
funciones públicas (las que aparecen en el `.h`) van sin `static`.

```c
/* helpers internos */
static int parse_field(const char *s);
static void normalize(char *buf);

/* función pública declarada en .h */
int pg_module_do_thing(void) { ... }
```

---

## `-Wshadow` con variables locales que ocultan parámetros

**Síntoma**:
```
error: declaration of 'pid' shadows a parameter [-Werror=shadow]
```

**Causa**: una variable local con el mismo nombre que un parámetro
(o una variable de scope externo).

**Fix**: renombrar la variable interna (`pid` → `pid_str`, `out` →
`local_out`, etc.). No hacer cast a void ni `__attribute__((unused))`
para silenciar.

---

## `-Wcast-align` al castear buffers `char*`

**Síntoma**:
```
error: cast increases required alignment of target type [-Werror=cast-align]
```

**Causa**: castear un `char *buf` (alignment 1) a un puntero a struct
(alignment 4/8) puede generar accesos desalineados en algunas arquitecturas.

**Fix**: usar `memcpy(&dest, buf, sizeof(dest))` en lugar de cast directo.

```c
/* MAL */
struct foo *f = (struct foo *)buf;

/* BIEN */
struct foo f;
memcpy(&f, buf, sizeof(f));
```

---

## ASAN reporta leaks que ocultan el output de Unity en RED

**Síntoma**: durante el ciclo TDD, las assertions fallidas ocultan su
mensaje porque ASAN imprime su reporte de leaks primero/encima.

**Causa**: cuando un `TEST_ASSERT_*` de Unity falla, sale del test con
`longjmp`, saltándose el cleanup (free, destroy). ASAN detecta los recursos
no liberados y aborta con un dump grande que opaca el resumen de Unity.

**Fix rápido para iterar**: usar `make test-quick` (definido en el
Makefile) que setea `ASAN_OPTIONS=detect_leaks=0`. Las assertions se ven
claras. `make test` mantiene leak detection para verificación final.

```bash
make test-quick   # iteración RED-GREEN sin ruido de leaks
make test         # verificación final, falla si hay leaks reales
```

---

## Unity vendored falla con `-Werror`

**Síntoma**: warnings dentro de `tests/unity/unity.c` se vuelven errores
si compila con los flags del proyecto.

**Causa**: Unity es código vendored third-party, no escrito bajo nuestras
restricciones.

**Fix**: regla específica para `unity.o` con flags mínimas, sin heredar
`$(CFLAGS)` del proyecto:

```makefile
$(BUILD_DIR)/unity.o: $(UNITY_DIR)/unity.c | $(BUILD_DIR)
	$(CC) -std=c11 -O1 -g3 -I$(UNITY_DIR) -c $< -o $@
```
