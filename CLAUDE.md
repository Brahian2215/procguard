# ProcGuard — Instrucciones del Proyecto

## Qué es este proyecto

ProcGuard es un sistema de gobernanza activa de procesos para Linux que
implementa el ciclo observación → detección → decisión → acción → registro.
Es el proyecto final del curso de Sistemas Operativos (2026-1, U. de Antioquia).
El diseño completo está en `docs/ProcGuard_Proyecto_Final.pdf`.
Léelo únicamente cuando lo pida explícitamente o cuando necesites la
especificación de un módulo concreto. No lo cargues en cada sesión.

## Restricciones técnicas inviolables

- Lenguaje: C estándar C11. Nada de C++, Rust, ni extensiones GNU gratuitas.
- Plataforma: Linux (kernel 5.x+) con cgroups v2 unified hierarchy.
- Dependencias externas permitidas: solo ncursesw (TUI), inih (INI parsing,
  vendored en `src/common/inih/`) y cJSON (JSON, vendored en
  `src/common/cjson/`). Cualquier otra dependencia requiere mi aprobación
  explícita.
- Compilación: debe compilar limpio con `-Wall -Wextra -Werror`. Sin
  excepciones. Un warning es un error.
- Threading: pthreads POSIX. No uses C11 threads ni OpenMP.
- Build: Make. No introduzcas CMake, Meson ni otros.

## Arquitectura (siete módulos)

- M1 Data Collector (`src/collector/`): lee procfs, identifica procesos por
  (pid, starttime). Procesos desaparecidos se conservan G=10 ciclos de gracia
  antes de liberar su buffer circular (permite snapshots forenses del historial).
- M2 Sample Store (`src/store/`): buffer circular de N muestras por proceso.
- M3 Metrics Engine (`src/metrics/`): calcula CPU%, RSS, tasas I/O, red.
- M4 Alert & Governance (`src/alert/`): políticas, persistencia, histéresis,
  cooldown, escalamiento. Antes de ejecutar cualquier acción correctiva,
  revalida que el par (pid, starttime) sigue siendo el mismo proceso que
  generó la alerta (previene actuar sobre PIDs reciclados).
- M5 Security Engine (`src/security/`): cuatro heurísticas (port scan,
  camuflados, enumeración, huérfanos anómalos).
- M6 TUI (`src/tui/`): ncurses, hilo dedicado.
- M7 Report (`src/report/`): JSON lines + snapshots + HTML.

Modelo de concurrencia:
- Modo interactivo: tres hilos (gobernanza, inotify listener, TUI) con
  comunicación por tres mutex independientes.
- Modo daemon: dos hilos (gobernanza + inotify; el hilo TUI no se instancia).
Nunca un hilo adquiere más de un mutex a la vez. Detalle en sección 5.11 del PDF.

Ciclo de gobernanza (10 pasos en orden fijo):
1. Consumir colas (comandos TUI encolados + eventos inotify encolados)
2. Recolectar (lectura procfs, best-effort por proceso)
3. Calcular métricas derivadas
4. Evaluar rendimiento (métricas vs políticas)
5. Evaluar seguridad (heurísticas acumulativas + eventos inotify)
6. Validar (revalidar pid+starttime; comprobar protecciones)
7. Actuar (ejecutar acción escalada, o registrar en dry-run)
8. Capturar (snapshot forense si aplica)
9. Registrar (log JSON lines + syslog)
10. Visualizar (depositar resultados en estructura compartida para hilo TUI)

## Reglas de proceso (NO NEGOCIABLES)

1. **Plan primero, código después.** Antes de escribir o modificar código,
   presenta un plan breve: qué archivos vas a tocar, qué funciones vas a
   crear/cambiar, qué tests vas a escribir. Espera mi OK antes de ejecutar.

2. **TDD estricto.** Para toda función no trivial: escribe el test primero,
   ejecútalo y confirma que falla, escribe la implementación mínima,
   ejecuta y confirma que pasa, refactoriza, vuelve a correr tests.
   No existe código sin test previo.

3. **Compilar y testear tras cada cambio.** Después de cada modificación,
   ejecuta `make asan && make test`. Si algo falla, arregla antes de
   continuar. Nunca avances con el árbol en estado roto.

4. **Commits frecuentes.** Commit tras cada tarea que deja el árbol verde.
   Mensajes en formato imperativo ("add collector skeleton", no "added...").
   Nunca commits que rompen el build.

5. **Scope discipline.** Haz exactamente lo pedido, nada más. Si ves algo
   que deberías cambiar fuera del scope, anótalo en `docs/TODO.md` y sigue.

6. **Cuestiona malas decisiones.** Si detectas que lo que pido llevará a
   mala arquitectura, dimelo antes de implementarlo. Mejor perder 5 minutos
   discutiendo que 5 horas rehaciendo.

7. **No inventes APIs.** Antes de usar una función de sistema (syscall o
   libc), verifica su firma exacta en el manual. En C, asumir hace crashes.

## Estándares de código

- Funciones: máximo 50 líneas. Si pasa, refactoriza.
- Un archivo `.c` tiene su header `.h` correspondiente. Headers con guards
  `#ifndef PG_MODULE_NAME_H`.
- Nombres: `snake_case` para funciones y variables, `UPPER_CASE` para
  macros y constantes, prefijo `pg_` para funciones públicas de módulo.
- Manejo de errores: toda función que puede fallar retorna un `int` con
  código de error. No uses `errno` como canal de retorno. Documenta qué
  errores retorna cada función en el header.
- Sin `malloc` sin su `free` correspondiente en la misma sesión de diseño.
  Ownership explícito y documentado en cada header.
- Sin `goto` excepto para cleanup en funciones largas (patrón estándar).
- Sin magic numbers. Usa constantes con nombre o macros.
- Comentarios: explica el porqué, no el qué. El código debe explicarse solo.

## Testing

- Framework: Unity (single-header, lo vendoramos en `tests/unity/`). No
  escribas tu propio framework de testing.
- Todo módulo tiene `tests/unit/test_<modulo>.c`.
- Tests de integración usan procfs sintético en `/tmp/pg_test_proc/`.
  El path base de `/proc` debe ser configurable en el recolector.
- Estructura mínima del procfs sintético por proceso:
  ```
  /tmp/pg_test_proc/
  └── <pid>/
      ├── stat    # formato idéntico a /proc/[pid]/stat (52 campos posicionales)
      ├── statm   # 7 campos: size resident shared text lib data dt
      └── io      # pares clave: valor (rchar, wchar, read_bytes, write_bytes)
  ```
  Campos mínimos requeridos en `stat` (posición 1-indexed):
  pid(1) comm(2) state(3) ppid(4) utime(14) stime(15) starttime(22) tty_nr(7).
  El resto de campos puede ser 0. `comm` va entre paréntesis: `(bash)`.
  Parser correcto para `comm`: buscar el último `)` en la línea (el nombre puede
  contener espacios y paréntesis internos); extraer entre el primer `(` y ese `)`.
- Build de tests: `make test` (lo definirás cuando exista el primer test).
- Antes de declarar "terminado" un módulo: debe pasar valgrind sin leaks y
  ASAN sin errores.

## Prohibiciones específicas

- No uses `system()`, `popen()` para lógica crítica (sí en tests donde
  simplifica).
- No silencies warnings con `__attribute__((unused))` salvo que sea
  inevitable y lo comentes.
- No uses variables globales mutables. Pasa estado explícitamente.
- No introduzcas threading donde no esté arquitectónicamente justificado.
  El modelo de tres hilos está decidido.
- No optimices prematuramente. Primero correcto, después rápido.

## Orden de desarrollo de módulos

Las dependencias de datos imponen este orden:
M1 → M2 → M3 → M4 → M5 (paralelo con M4) → M6 → M7

- M1 es prerequisito de todos: produce los pg_raw_sample_t
- M2 requiere M1: almacena sus muestras en buffer circular
- M3 requiere M2: necesita al menos 2 muestras para calcular deltas
- M4 requiere M3: evalúa métricas calculadas contra políticas
- M5 requiere M1+M2 (análisis acumulativo) + hilo inotify (notificaciones push)
- M6 requiere M4: visualiza alertas activas y métricas
- M7 requiere M4+M5: registra alertas y eventos de seguridad

No implementes un módulo sin que sus prerequisitos pasen sus propios tests.

## Flujo de trabajo

1. Empiezo una sesión con una tarea concreta.
2. Entras en plan mode, lees lo mínimo necesario, presentas plan.
3. Apruebo o ajusto.
4. Implementas siguiendo TDD, commiteando por tarea verde.
5. Al final, actualizas `docs/STATE.md` con qué quedó hecho, decisiones
   tomadas, y qué sigue.

## Archivos de memoria del proyecto

- `docs/STATE.md`: estado actual, módulos completos, próximos pasos.
  Lee este archivo al inicio de cada sesión nueva.
- `docs/DECISIONS.md`: decisiones arquitectónicas con justificación.
  Consulta cuando tengas duda sobre el porqué de algo.
- `docs/TODO.md`: tareas pendientes fuera del scope actual.

### Template obligatorio para STATE.md

Al cerrar cada sesión, actualiza `docs/STATE.md` con este formato exacto:

```
## Última actualización
Sesión N — YYYY-MM-DD — descripción breve de qué se completó

## Módulos completados
- Mód X: funciones implementadas, tests passing (valgrind + ASAN limpios)

## Tests pasando
- tests/unit/test_X.c (N tests: N passed, 0 failed)

## Última acción ejecutada
<mensaje del último commit> (<hash corto>)

## Próximos pasos
1. Primera tarea de la sesión siguiente
2. Segunda tarea (si ya conocida)
```

## Cómo construir y correr

```bash
make debug     # build con símbolos y sin optimización
make asan      # build con AddressSanitizer
make test      # corre todos los tests (cuando existan)
make valgrind  # corre el binario bajo valgrind --leak-check=full
make clean
```

## Fuera de alcance (deliberadamente)

- Portabilidad a BSD, macOS o Windows.
- Soporte para cgroups v1.
- Internacionalización.
- Interfaz gráfica fuera de TUI.
- Modo cliente-servidor o API REST.
