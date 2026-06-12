# ProcGuard — Instrucciones del Proyecto

Sistema de gobernanza activa de procesos para Linux (observación →
detección → decisión → acción → registro). Proyecto final Sistemas
Operativos 2026-1, U. de Antioquia. Spec completa en
`docs/ProcGuard_Proyecto_Final.pdf` (cárgala solo cuando la necesites).

## Restricciones técnicas inviolables

- **C estándar C11**. Sin C++, Rust, ni extensiones GNU gratuitas. `_GNU_SOURCE`
  permitido donde haga falta.
- **Linux 5.x+** con cgroups v2 unified hierarchy.
- **Dependencias externas permitidas**: `ncursesw` (TUI), `inih` (vendored en
  `src/common/inih/`), `cJSON` (vendored en `src/common/cjson/`). Cualquier
  otra requiere aprobación explícita.
- **Compilación limpia**: `-Wall -Wextra -Werror -Wshadow -Wpointer-arith
  -Wcast-align -Wstrict-prototypes -Wmissing-prototypes`. Un warning es
  un error.
- **Threading**: pthreads POSIX. No C11 threads ni OpenMP.
- **Build**: Make. No CMake, no Meson.

## Arquitectura (siete módulos)

- **M1 Data Collector** (`src/collector/`): procfs → `pg_raw_sample_t[]`.
  Identifica procesos por `(pid, starttime)`.
- **M2 Sample Store** (`src/store/`): buffer circular de N muestras por
  proceso; gestiona período de gracia G=10 ciclos.
- **M3 Metrics Engine** (`src/metrics/`): CPU%, RSS, tasas I/O, red.
  Funciones puras.
- **M4 Alert & Governance** (`src/alert/`): políticas (inih), histéresis,
  cooldown, escalamiento. Revalida `(pid, starttime)` antes de actuar.
- **M5 Security Engine** (`src/security/`): cuatro heurísticas (port scan,
  camuflados, enumeración, huérfanos anómalos).
- **M6 TUI** (`src/tui/`): ncurses, hilo dedicado.
- **M7 Report** (`src/report/`): JSON lines + snapshots + HTML (cJSON).

Orden de desarrollo por dependencias: M1 → M2 → M3 → M4 → (M5‖M6) → M7.

**Concurrencia**: modo interactivo 3 hilos (gobernanza, inotify, TUI);
modo daemon 2 hilos (sin TUI). Mutex independientes, ningún hilo toma más
de uno simultáneamente. Detalle en `docs/plans/slice-4-concurrency.md`.

**Ciclo de gobernanza (10 pasos)**: consumir colas → recolectar → calcular →
evaluar rendimiento → evaluar seguridad → validar → actuar → capturar →
registrar → visualizar.

## Reglas de proceso

1. **Plan primero para cambios no triviales.** Lee el plan del slice
   (`docs/plans/slice-N.md`) si existe. Identifica ambigüedades antes de
   codear — pregunta si algo no está claro. Decisiones arquitectónicas nuevas
   se registran como ADR en `docs/DECISIONS.md` antes del commit. Para fixes
   pequeños o refactors obvios, ir directo.

2. **TDD para funciones con lógica propia.** Parser de procfs, buffer
   circular, aritmética de métricas, evaluación de políticas: test primero,
   falla, implementa, pasa, refactoriza. Glue code, wrappers triviales y
   comparadores de pocas líneas **no** requieren test dedicado — se validan
   con valgrind + inspección visual del integrador.

3. **Compilar y testear tras cada cambio.** `make asan && make test`. Si
   algo falla, arreglar antes de continuar. Usa `make test-quick` para
   iterar en RED-GREEN sin ruido de leaks; `make test` para verificación
   final.

4. **Commits por tarea verde.** Mensaje imperativo ("add X", no "added X").
   Nunca commits que rompen el build.

5. **Funciones ≤50 líneas.** `make lint-funclen` antes del commit final.
   Refactoriza extrayendo helpers `static` si se pasa.

6. **No inventes APIs.** Verifica firmas de syscalls/libc en `man` antes
   de usarlas. Si un `sscanf` o `ioctl` no es obvio, compila un snippet
   aparte con los flags reales del proyecto antes de incorporarlo.

7. **Cuestiona malas decisiones.** Si lo que pido lleva a mala arquitectura,
   dímelo antes de implementar.

8. **Scope discipline.** Haz exactamente lo pedido. Cosas fuera de scope
   van a la sección "Deuda técnica" de `docs/STATE.md`, no al commit actual.

## Convenciones de código

- **Estilo**: `snake_case` para funciones/variables, `UPPER_CASE` para
  macros, prefijo `pg_` para funciones públicas de módulo.
- **Headers**: guards `#ifndef PG_MODULE_NAME_H`. Un `.c` → un `.h`.
- **Errores**: toda función que puede fallar retorna `int` con código
  (`PG_OK`, `PG_ERR_PARSE`, `PG_ERR_IO`, `PG_ERR_MEM`). No `errno` como
  canal de retorno.
- **API pública defensiva**: funciones públicas validan NULL y retornan
  `PG_ERR_PARSE`. `pg_X_destroy(NULL)` es no-op (idiomático estilo `free`).
- **Structs públicas**: append-only mientras no exista ABI estable.
  Campos nuevos en 0 cuando la fuente subyacente falla.
- **Ownership**: cada `malloc` tiene su `free` en la misma sesión de
  diseño. Documentado en el header.
- **Sin globals mutables**. Pasa estado explícitamente.
- **Sin magic numbers**. Constantes con nombre o macros.
- **Sin `goto`** excepto cleanup en funciones largas.
- **Comentarios**: explica el porqué, no el qué.
- **Tolerancia de tests float**: `TEST_ASSERT_FLOAT_WITHIN(0.001f, ...)`.
  1 ULP es frágil ante reordenamientos de cálculo.

## Testing

- **Framework**: Unity (vendored en `tests/unity/`).
- **Procfs sintético** en `/tmp/pg_test_proc/` con estructura:
  ```
  /tmp/pg_test_proc/<pid>/
      stat    # 52 campos posicionales
      statm   # 7 campos: size resident shared text lib data dt
      io      # pares clave: valor (rchar, wchar, read_bytes, write_bytes)
  ```
  Campos mínimos en `stat` (1-indexed): pid(1) comm(2) state(3) ppid(4)
  tty_nr(7) utime(14) stime(15) starttime(22). Resto puede ser 0.
  `comm` entre paréntesis: `(bash)`; parser usa el ÚLTIMO `)` como
  delimitador (puede contener paréntesis internos).
- **Antes de declarar un módulo terminado**: `make asan` + `make test` +
  `make valgrind` verdes.

## Archivos del proyecto

- `docs/STATE.md` — estado, roadmap, próximos pasos, deuda técnica.
- `docs/DECISIONS.md` — ADRs (solo trade-offs reales con consecuencias).
- `docs/plans/slice-N.md` — plan del slice activo.
- `docs/MAKEFILE_GOTCHAS.md` — warnings no obvios y sus fixes.
- `docs/plans/slice-4-concurrency.md` — modelo de tres hilos; leer antes
  de Slice 4.

Al cerrar sesión, actualizar `docs/STATE.md`.

## Prohibiciones

- No `system()`/`popen()` para lógica crítica (sí en tests donde
  simplifica).
- No `__attribute__((unused))` para silenciar warnings — arregla el warning.
- No threading donde no esté arquitectónicamente justificado.
- No optimices prematuramente.

## Comandos

```bash
make debug           # símbolos, sin optimización
make asan            # AddressSanitizer + UBSAN
make tsan            # ThreadSanitizer (races; build sin ASAN). Activa en Slice 5
make test            # tests con leak detection
make test-quick      # tests sin leak detection (iteración RED-GREEN)
make valgrind        # memcheck (requiere build sin ASAN)
make lint-funclen    # flaggea funciones >50 líneas
make clean
```

Workflow valgrind: `make clean && make debug && make valgrind` (ASan y
valgrind no conviven).

## Fuera de alcance

Portabilidad BSD/macOS/Windows, cgroups v1, i18n, GUI fuera de TUI, modo
cliente-servidor o REST.
