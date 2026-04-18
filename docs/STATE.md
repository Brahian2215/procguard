# Estado del Proyecto

## Última actualización
Sesión 1 — 2026-04-17 — arranque: CLAUDE.md corregido (A1-A4, B1-B3) y plan de Slice 1 creado

## Módulos completados
Ninguno todavía.

## Tests pasando
Ninguno todavía.

## Última acción ejecutada
Setup inicial (commit pendiente al final de esta sesión).

## Próximos pasos
1. Crear `src/common/pg_types.h` (pg_proc_id_t, pg_raw_sample_t sin vmrss, códigos de error)
2. Crear `src/collector/collector.h` + `collector.c` con path base configurable
3. Crear `tests/unit/test_collector.c` con procfs sintético
4. Actualizar Makefile para build + `make test` funcionales
