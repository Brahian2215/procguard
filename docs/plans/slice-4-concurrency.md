# Modelo de concurrencia

Resumen de `ProcGuard_Proyecto_Final.pdf` §5.11 (pp. 21-22). Releer antes
de código que toque hilos o estructuras compartidas.

## Modelo de tres hilos

Tres naturalezas temporales distintas → tres hilos con responsabilidades
disjuntas:

- **Hilo gobernanza** — único que lee `/proc`, calcula métricas, evalúa
  políticas y ejecuta acciones. Al inicio de cada iteración consume
  comandos encolados por TUI (si está activo) y eventos encolados por
  inotify. Al final deposita resultados en la struct compartida.
- **Hilo inotify** — único que opera el descriptor de inotify. `read()`
  bloqueante; escribe cada evento (timestamp + wd + mask) en la cola
  protegida por mutex. El mapeo `wd → path` vive en el productor, no se
  duplica por evento.
- **Hilo TUI** — único que toca ncurses. `getch()` con timeout 100 ms
  (`halfdelay(1)`). Sólo lee la struct de resultados y escribe en la cola
  de comandos. **En modo daemon no se instancia** (queda con 2 hilos).

## Sincronización

Tres structs con mutex independientes. **Regla invariante**: ningún hilo
adquiere más de un mutex simultáneamente → deadlocks eliminados por
diseño. Si se necesitan datos de dos structs, accesos secuenciales
(lock A, copiar, unlock A; lock B, copiar, unlock B).

| Estructura | Escritor | Lector | Default §5.10 |
|---|---|---|---|
| Resultados del ciclo | Gobernanza | TUI | — |
| Cola eventos inotify | Inotify | Gobernanza | 256 |
| Cola comandos interactivos | TUI | Gobernanza | 32 |

Overflow de colas → **drop-oldest** + contador `dropped` (frescura sobre
completitud; en un flood los eventos recientes son los útiles).

## Reglas inviolables (resumen grep-able)

1. Ningún hilo adquiere más de un mutex simultáneamente.
2. Sólo gobernanza lee `/proc`, calcula, evalúa, actúa.
3. Sólo el hilo inotify toca el descriptor de inotify.
4. Sólo el hilo TUI toca ncurses.
5. Modo daemon → 2 hilos (gobernanza + inotify). Interactivo → 3.
6. `getch()` con timeout 100 ms en TUI.
7. Las tres colas/structs tienen mutex independientes.
8. M2, M3, M4 viven dentro del hilo de gobernanza → sin mutex propio.
