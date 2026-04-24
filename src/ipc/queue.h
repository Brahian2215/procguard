#ifndef PG_IPC_QUEUE_H
#define PG_IPC_QUEUE_H

#include "pg_types.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IPC Slice 4 — tres estructuras compartidas entre los hilos del modelo
 * `docs/plans/slice-4-concurrency.md`. Cada una tiene mutex propio; ningún
 * hilo adquiere más de uno simultáneamente (regla inviolable §5.11 del PDF).
 *
 * Slice 4 NO instancia los hilos; deja la infra lista para que Slice 5
 * cablee gobernanza + inotify y Slice 6 añada TUI.
 */

/*
 * pg_results_t — snapshot del último ciclo de gobernanza. Escritor: hilo
 * de gobernanza. Lector: hilo de TUI. Caller toma mutex vía lock/unlock
 * antes de leer o escribir campos. Slice 4 sólo establece mutex; el
 * payload (top processes, decisiones, stats) se añade cuando TUI lo
 * consuma en Slice 6.
 */
typedef struct {
    pthread_mutex_t mu;
} pg_results_t;

int  pg_results_init(pg_results_t *r);
void pg_results_destroy(pg_results_t *r);
int  pg_results_lock(pg_results_t *r);
int  pg_results_unlock(pg_results_t *r);

/*
 * pg_inotify_event_t — evento emitido por el hilo de inotify. Cada evento
 * lleva timestamp de captura (CLOCK_MONOTONIC ms), watch descriptor del
 * archivo vigilado, y la máscara inotify. El mapeo wd→path vive en el
 * hilo productor (no se duplica por evento).
 */
typedef struct {
    uint64_t timestamp_ms;
    int      wd;
    uint32_t mask;
} pg_inotify_event_t;

/*
 * Ring buffer FIFO de capacidad fija para eventos inotify. Default PDF
 * §5.10 = 256. Si el consumidor (gobernanza) no drena a tiempo y la cola
 * se llena, el próximo push descarta el evento más antiguo e incrementa
 * el contador `dropped`. Esta política privilegia frescura sobre
 * completitud: en un flood, los eventos más recientes son los más útiles
 * para detección en tiempo real.
 */
typedef struct {
    pthread_mutex_t     mu;
    pg_inotify_event_t *buf;
    size_t              capacity;
    size_t              head;    /* próximo pop */
    size_t              tail;    /* próximo push */
    size_t              count;
    size_t              dropped; /* total descartados por overflow */
} pg_inotify_event_queue_t;

int  pg_inotify_queue_init(pg_inotify_event_queue_t *q, size_t capacity);
void pg_inotify_queue_destroy(pg_inotify_event_queue_t *q);

/*
 * push: encola ev. Si la cola está llena, descarta el más antiguo y
 * ++dropped. Retorna PG_OK siempre (overflow no es error del caller).
 * PG_ERR_PARSE si q o ev == NULL.
 */
int pg_inotify_queue_push(pg_inotify_event_queue_t *q,
                          const pg_inotify_event_t *ev);

/*
 * pop: drena hasta `buf_cap` eventos en orden cronológico en buf.
 * *out_len recibe la cantidad real (<= buf_cap y <= q->count). Cola
 * vacía retorna PG_OK con *out_len=0.
 * PG_ERR_PARSE si q, buf u out_len == NULL o buf_cap == 0.
 */
int pg_inotify_queue_pop(pg_inotify_event_queue_t *q,
                         pg_inotify_event_t *buf, size_t buf_cap,
                         size_t *out_len);

/*
 * pg_command_t — comando emitido por el hilo de TUI al hilo de
 * gobernanza (quit, pause, resume, kill_pid). Slice 4 deja el enum
 * abierto; Slice 6 añadirá las acciones concretas.
 */
typedef enum {
    PG_CMD_NONE = 0,
    PG_CMD_QUIT,
    PG_CMD_PAUSE,
    PG_CMD_RESUME,
    PG_CMD_KILL_PID
} pg_command_kind_t;

typedef struct {
    pg_command_kind_t kind;
    int               arg;  /* pid para KILL_PID, 0 para el resto */
} pg_command_t;

/*
 * Ring buffer FIFO de comandos interactivos. Default PDF §5.10 = 32.
 * Mismas semánticas de overflow que la cola de inotify (drop-oldest).
 */
typedef struct {
    pthread_mutex_t  mu;
    pg_command_t    *buf;
    size_t           capacity;
    size_t           head;
    size_t           tail;
    size_t           count;
    size_t           dropped;
} pg_command_queue_t;

int  pg_command_queue_init(pg_command_queue_t *q, size_t capacity);
void pg_command_queue_destroy(pg_command_queue_t *q);
int  pg_command_queue_push(pg_command_queue_t *q, const pg_command_t *cmd);
int  pg_command_queue_pop(pg_command_queue_t *q,
                          pg_command_t *buf, size_t buf_cap,
                          size_t *out_len);

#endif /* PG_IPC_QUEUE_H */
