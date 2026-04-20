# Roadmap de Slices

Mapa direccional, no detallado. Cada slice se planifica en su propio
`docs/plans/slice-N.md` antes de ejecutar la primera sesión.

Orden derivado del grafo de dependencias del CLAUDE.md
(M1 → M2 → M3 → M4 → M5∥M6 → M7).

---

## Slice 1 — Vertical mínima ejecutable (en curso)

**Objetivo**: `./build/procguard` muestra top-5 procesos por CPU%.

- Sesión 1 ✅ M1 Collector básico + tests + Makefile (commit `3da86ff`)
- Sesión 2 ⏭️ M3 Metrics CPU% + tests
- Sesión 3 ⏭️ `src/main.c` real (dos scans, qsort, top-5) + valgrind verde

**Criterio de cierre**: `make valgrind` exit 0; binario visible con datos reales.

---

## Slice 2 — M2 Sample Store + extensión de M1

**Objetivo**: persistir las muestras del collector en buffer circular por
proceso y soportar el período de gracia G=10 ciclos.

- M2 Sample Store: `pg_store_init/insert/get_history/destroy`
  con buffer circular de N muestras por `pg_proc_id_t`
- M1 extendido: tracking de procesos vistos para gracia G=10 (procesos que
  desaparecen se conservan G ciclos antes de liberar su entry)
- M1 extendido: lectura de `/proc/[pid]/statm` → `pg_raw_sample_t.vmrss`
- M1 extendido: lectura de `/proc/[pid]/io` → tasas I/O en M3
- Filtrado de kernel threads (decisión: por flag explícito en collector init)

**Riesgo**: el cambio de `pg_raw_sample_t` impacta los tests existentes
de M1. Planificar el migration.

---

## Slice 3 — M4 Alert & Governance básico

**Objetivo**: políticas estáticas con histéresis y cooldown evalúan métricas
de M3 y registran alertas (sin acciones correctivas todavía).

- M4 motor de políticas: `pg_policy_load` (vía inih), `pg_policy_evaluate`
- Histéresis (umbral activación vs desactivación) y cooldown (tiempo mínimo
  entre alertas consecutivas para el mismo proceso)
- Revalidación `pid+starttime` antes de actuar (CLAUDE.md regla)
- M4 modo dry-run: alertas se registran pero no se ejecutan acciones
- Refactor del Makefile: introducir `build/tests/` con objetos ASAN
  reutilizables (deuda técnica de Slice 1, ver TODO.md)

---

## Slice 4 — M5 Security + threading + inotify

**Objetivo**: introducir el modelo de tres hilos y las heurísticas de
seguridad. **Prerequisito crítico**: `docs/concurrency.md` extraído del
PDF sección 5.11 antes de iniciar.

- Hilo gobernanza (loop de 10 pasos del CLAUDE.md)
- Hilo inotify listener (eventos asíncronos de filesystem)
- M5 cuatro heurísticas: port scan, camuflados, enumeración, huérfanos anómalos
- Mutex independientes (sin nesting), colas inter-hilo

---

## Slice 5 — M6 TUI

**Objetivo**: interfaz ncurses con hilo dedicado.

- Tercer hilo (TUI) en modo interactivo
- Estructura compartida hilo-gobernanza ↔ hilo-TUI
- Vistas: top procesos, alertas activas, histórico

---

## Slice 6 — M7 Report + acciones correctivas

**Objetivo**: persistencia de eventos y ejecución de acciones M4 (no sólo
dry-run).

- M7 Report: JSON lines (eventos), snapshots forenses, HTML resumen
- M4 acciones: niceness, OOM score adjust, cgroup throttling, kill
- Escalamiento (warn → throttle → kill con timeouts)

---

## Slice 7 — Hardening + modo daemon

**Objetivo**: robustez de producción.

- Modo daemon: dos hilos (sin TUI)
- Reconfiguración en caliente (SIGHUP recarga policies)
- Tests de integración con procfs real (deuda Slice 1)
- Migración a `mkdtemp` para fixtures de tests (deuda Slice 1)
- Benchmark y profiling (sólo si métricas indican necesidad)

---

## Notas

- El detalle de cada slice se cristaliza en su `docs/plans/slice-N.md`
  cuando se vaya a empezar, no antes (evita planning waste si las
  prioridades cambian).
- Slices 2-7 pueden reordenarse según necesidad académica (entrega parcial,
  feedback del profesor, etc.). El orden listado respeta dependencias
  fuertes (M2 antes de M3, M3 antes de M4, etc.).
- Cada slice cierra con `make asan && make test && make valgrind` verdes y
  un commit imperativo.
