# Slice 2 — M2 Sample Store + extensión de M1

## Objetivo

Persistir muestras por proceso en buffer circular y soportar el período de
gracia G=10 ciclos. Extender M1 con `vmrss_bytes`, counters I/O y filtro
opcional de kernel threads.

## Estado a fecha

**Hecho** (commits en `git log`):
- `pg_raw_sample_t` extendido con `vmrss_bytes`, `rchar`, `wchar`,
  `read_bytes`, `write_bytes`.
- `pg_collector_init` acepta `bool skip_kernel_threads`.
- `read_proc_statm` y `read_proc_io` best-effort en scan loop.
- Filtro `ppid == 2 || pid == 2` cuando el flag está activo.
- M2: `pg_store_init`, `pg_store_insert`, `pg_store_get_history`,
  `pg_store_destroy`. Buffer circular por `pg_proc_id_t` con capacidad
  configurable (N=16 por defecto en el integrador).
- 20 tests verdes; ASAN+UBSAN+valgrind limpios.

**Pendiente:**
- `pg_store_tick(store, grace_cycles)` — incrementa `absent_cycles` por
  entry que no recibió insert, libera entries que **exceden** `grace_cycles`.
  En insert resetea `absent_cycles = 0` y marca `seen_this_tick = true`.
- Cablear store en `src/main.c`: crear al inicio, insertar scans, llamar
  `tick` entre medias, destruir al final. Output visible no cambia
  (top-5 por CPU% sigue siendo el criterio).
- Link `src/store/store.c` en la regla de `$(BUILD_DIR)/procguard`.

## API pendiente

```c
/* Avanza el ciclo: incrementa absent_cycles en entries no vistas desde
 * el último tick; libera entries con absent_cycles > grace_cycles.
 * Las vistas resetean absent_cycles a 0 y su flag seen_this_tick.
 *
 * Semántica: entry insertada en ciclo k y no reinsertada sobrevive hasta
 * el tick del ciclo k+grace_cycles inclusive; se libera en el siguiente.
 */
int pg_store_tick(pg_store_t *store, unsigned int grace_cycles);
```

Liberación compacta el array (swap-con-último) para mantener `entries`
denso.

## Tests pendientes (3, en `test_store.c`)

1. `test_tick_grace_boundary` — insertar A; 10 ticks sin reinsert →
   history aún devuelve. Tick 11 → history vacío.
2. `test_tick_resets_counter_on_reinsert` — insertar A; 5 ticks; reinsertar;
   9 ticks más → history sigue devolviendo.
3. `test_tick_frees_multiple_expired` — 3 ids insertados; 12 ticks sin
   inserts → los 3 liberados. ASAN valida no-leaks.

## Criterio de cierre del slice

- `make asan && make test && make lint-funclen` verdes.
- `make clean && make debug && make valgrind` verde.
- 23 tests totales (8 collector + 5 metrics + 10 store).
- ADR-007 (gracia G=10 en M2, no M1) ya registrado.
- STATE.md actualizado con "Slice 2 cerrado" y próximo paso Slice 3.

## Riesgos

- `/proc/[pid]/io` requiere privilegios suficientes; los counters quedan en
  0 como usuario normal. Documentado en STATE.md. No afecta tests (fixtures
  sintéticos controlan el contenido).
- Buffer global (`/proc/stat`, `/proc/meminfo`) queda diferido. Slice 3+ lo
  abordará cuando M4 lo necesite.
