#ifndef STATS_H
#define STATS_H

/*
 * Estadísticas en memoria compartida (mmap MAP_SHARED creada por el master
 * antes del fork). Cada worker escribe solo en su slot: contadores atómicos
 * y una tabla de servidores publicada periódicamente con un seqlock. El
 * master agrega y sirve JSON por un socket UNIX.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATS_MAX_WORKERS 128
#define STATS_MAX_SERVERS 256

typedef struct {
    char backend[64];
    char server[64];
    int up;
    int active;
    int load_known;
    double load;
    uint64_t requests;
    uint64_t failures;
    int idle;
} stats_server_t;

enum { ST_1XX, ST_2XX, ST_3XX, ST_4XX, ST_5XX, ST_NCLASS };

typedef struct {
    _Alignas(64) atomic_int pid;
    atomic_uint_fast64_t requests;
    atomic_uint_fast64_t accepted;
    atomic_int_fast64_t active_conns;
    atomic_uint_fast64_t tls_handshakes;
    atomic_uint_fast64_t upstream_connects;
    atomic_uint_fast64_t upstream_reuses;
    atomic_uint_fast64_t upstream_retries;
    atomic_uint_fast64_t reloads;
    atomic_uint_fast64_t log_dropped;
    atomic_uint_fast64_t resp[ST_NCLASS];
    atomic_uint_fast64_t buffers_in_use;
    atomic_uint seq;
    int nservers;
    stats_server_t servers[STATS_MAX_SERVERS];
} stats_worker_t;

typedef struct {
    uint64_t start_wall_ms;
    uint64_t start_mono_ms;
    int nworkers;
    atomic_uint_fast64_t reloads_ok;
    atomic_uint_fast64_t reloads_failed;
    atomic_uint_fast64_t worker_restarts;
    stats_worker_t w[];
} stats_shm_t;

stats_shm_t *stats_create(int nworkers);
void stats_destroy(stats_shm_t *s);

static inline void stat_inc(atomic_uint_fast64_t *c)
{
    atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
}

/* Publicación con seqlock desde el worker. */
void stats_publish_begin(stats_worker_t *w);
void stats_publish_end(stats_worker_t *w);

/* Construye el JSON agregado. Devuelve buffer malloc'ado. */
char *stats_json(stats_shm_t *s, size_t *len);

#endif
