#ifndef BACKEND_POOL_H
#define BACKEND_POOL_H

/*
 * Backends y selección de servidor: round_robin, weighted (smooth WRR),
 * least_conn y least_load (cabecera X-Backend-Load + power of two choices).
 *
 * El estado de salud (up/fails/oks) es atómico porque lo actualizan el hilo
 * del event loop (fallos pasivos) y el hilo de health checks (sondas). El
 * resto de campos solo los toca el hilo del event loop.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "config.h"

#define LOAD_EMA_ALPHA 0.3
#define LOAD_STALE_MS 5000u

typedef struct server {
    char addr_str[CFG_ADDR_MAX];
    struct sockaddr_storage sa;
    socklen_t salen;
    int idx;
    int weight;
    int cur_weight; /* smooth weighted round robin */

    int active; /* peticiones en curso en este worker */
    double load;
    uint64_t load_ts;
    bool load_known;

    atomic_int up;
    atomic_int fails;
    atomic_int oks;
    uint64_t down_since; /* para recuperación pasiva sin health activo */

    uint64_t requests;
    uint64_t failures;

    /* pool de conexiones keep-alive ociosas (lo gestiona connection.c) */
    void *idle_head;
    int idle_count;
} server_t;

typedef struct backend {
    char name[CFG_NAME_MAX];
    strategy_t strategy;
    int max_idle;
    cfg_health_t health;
    server_t *servers;
    int nservers;
    unsigned rr;
    uint64_t rng;
} backend_t;

/* Inicializa desde la config (resuelve direcciones). 0 ok / -1. */
int backend_init(backend_t *b, const cfg_backend_t *cfg, char *err, size_t errlen);
void backend_fini(backend_t *b);

/*
 * Elige un servidor sano, excluyendo los bits de exclude (servidores ya
 * probados en esta petición). NULL si no hay ninguno disponible.
 */
server_t *backend_pick(backend_t *b, uint64_t exclude, uint64_t now);

bool server_is_up(backend_t *b, server_t *s, uint64_t now);
void server_report_load(server_t *s, double v, uint64_t now);
/* Resultado pasivo desde el event loop. Devuelve true si cambió de estado. */
bool server_passive_fail(backend_t *b, server_t *s, uint64_t now);
void server_passive_ok(server_t *s);
/* Resultado de una sonda activa. Devuelve true si cambió de estado. */
bool server_probe_result(backend_t *b, server_t *s, bool ok);

/* Parsea el valor de X-Backend-Load. true si válido (0.0 - 1.0). */
bool parse_load(const char *v, size_t len, double *out);

#endif
