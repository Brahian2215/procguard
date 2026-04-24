#include "queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── pg_results_t ────────────────────────────────────────────────── */

int pg_results_init(pg_results_t *r)
{
    if (!r) return PG_ERR_PARSE;
    if (pthread_mutex_init(&r->mu, NULL) != 0) return PG_ERR_MEM;
    return PG_OK;
}

void pg_results_destroy(pg_results_t *r)
{
    if (!r) return;
    pthread_mutex_destroy(&r->mu);
}

int pg_results_lock(pg_results_t *r)
{
    if (!r) return PG_ERR_PARSE;
    if (pthread_mutex_lock(&r->mu) != 0) return PG_ERR_IO;
    return PG_OK;
}

int pg_results_unlock(pg_results_t *r)
{
    if (!r) return PG_ERR_PARSE;
    if (pthread_mutex_unlock(&r->mu) != 0) return PG_ERR_IO;
    return PG_OK;
}

/* ── pg_inotify_event_queue_t ────────────────────────────────────── */

int pg_inotify_queue_init(pg_inotify_event_queue_t *q, size_t capacity)
{
    if (!q || capacity == 0) return PG_ERR_PARSE;
    q->buf = calloc(capacity, sizeof(*q->buf));
    if (!q->buf) return PG_ERR_MEM;
    q->capacity = capacity;
    q->head = q->tail = q->count = q->dropped = 0;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->buf);
        q->buf = NULL;
        return PG_ERR_MEM;
    }
    return PG_OK;
}

void pg_inotify_queue_destroy(pg_inotify_event_queue_t *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
    q->buf = NULL;
}

int pg_inotify_queue_push(pg_inotify_event_queue_t *q,
                          const pg_inotify_event_t *ev)
{
    if (!q || !ev) return PG_ERR_PARSE;
    pthread_mutex_lock(&q->mu);
    if (q->count == q->capacity) {
        q->head = (q->head + 1) % q->capacity;
        q->dropped++;
        q->count--;
    }
    q->buf[q->tail] = *ev;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_mutex_unlock(&q->mu);
    return PG_OK;
}

int pg_inotify_queue_pop(pg_inotify_event_queue_t *q,
                         pg_inotify_event_t *buf, size_t buf_cap,
                         size_t *out_len)
{
    if (!q || !buf || buf_cap == 0 || !out_len) return PG_ERR_PARSE;
    pthread_mutex_lock(&q->mu);
    size_t n = q->count < buf_cap ? q->count : buf_cap;
    for (size_t i = 0; i < n; i++) {
        buf[i] = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
    }
    q->count -= n;
    *out_len = n;
    pthread_mutex_unlock(&q->mu);
    return PG_OK;
}

/* ── pg_command_queue_t ──────────────────────────────────────────── */

int pg_command_queue_init(pg_command_queue_t *q, size_t capacity)
{
    if (!q || capacity == 0) return PG_ERR_PARSE;
    q->buf = calloc(capacity, sizeof(*q->buf));
    if (!q->buf) return PG_ERR_MEM;
    q->capacity = capacity;
    q->head = q->tail = q->count = q->dropped = 0;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->buf);
        q->buf = NULL;
        return PG_ERR_MEM;
    }
    return PG_OK;
}

void pg_command_queue_destroy(pg_command_queue_t *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
    q->buf = NULL;
}

int pg_command_queue_push(pg_command_queue_t *q, const pg_command_t *cmd)
{
    if (!q || !cmd) return PG_ERR_PARSE;
    pthread_mutex_lock(&q->mu);
    if (q->count == q->capacity) {
        q->head = (q->head + 1) % q->capacity;
        q->dropped++;
        q->count--;
    }
    q->buf[q->tail] = *cmd;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_mutex_unlock(&q->mu);
    return PG_OK;
}

int pg_command_queue_pop(pg_command_queue_t *q,
                         pg_command_t *buf, size_t buf_cap,
                         size_t *out_len)
{
    if (!q || !buf || buf_cap == 0 || !out_len) return PG_ERR_PARSE;
    pthread_mutex_lock(&q->mu);
    size_t n = q->count < buf_cap ? q->count : buf_cap;
    for (size_t i = 0; i < n; i++) {
        buf[i] = q->buf[q->head];
        q->head = (q->head + 1) % q->capacity;
    }
    q->count -= n;
    *out_len = n;
    pthread_mutex_unlock(&q->mu);
    return PG_OK;
}
