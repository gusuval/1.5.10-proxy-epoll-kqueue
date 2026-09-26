#include "stats.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static size_t shm_size(int nworkers)
{
    return sizeof(stats_shm_t) + (size_t)nworkers * sizeof(stats_worker_t);
}

stats_shm_t *stats_create(int nworkers)
{
    size_t sz = shm_size(nworkers);
    void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (m == MAP_FAILED)
        return NULL;
    memset(m, 0, sz);
    stats_shm_t *s = m;
    s->nworkers = nworkers;
    s->start_wall_ms = wall_ms();
    s->start_mono_ms = mono_ms();
    return s;
}

void stats_destroy(stats_shm_t *s)
{
    if (s)
        munmap(s, shm_size(s->nworkers));
}

void stats_publish_begin(stats_worker_t *w)
{
    atomic_fetch_add_explicit(&w->seq, 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

void stats_publish_end(stats_worker_t *w)
{
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&w->seq, 1, memory_order_relaxed);
}

typedef struct {
    char *p;
    size_t len, cap;
} sb_t;

static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_printf(sb_t *b, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if ((size_t)n < b->cap - b->len) {
            b->len += (size_t)n;
            return;
        }
        size_t nc = b->cap * 2 + (size_t)n;
        char *np = realloc(b->p, nc);
        if (!np)
            return;
        b->p = np;
        b->cap = nc;
    }
}

typedef struct {
    stats_server_t s;
    int up_workers, seen_workers;
    double load_sum;
    int load_n;
} agg_t;

/* Copia consistente de la tabla de servidores de un worker. */
static int snapshot(stats_worker_t *w, stats_server_t *out)
{
    for (int tries = 0; tries < 100; tries++) {
        unsigned s1 = atomic_load_explicit(&w->seq, memory_order_acquire);
        if (s1 & 1u)
            continue;
        int n = w->nservers;
        if (n < 0 || n > STATS_MAX_SERVERS)
            n = 0;
        memcpy(out, w->servers, (size_t)n * sizeof(stats_server_t));
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&w->seq, memory_order_relaxed) == s1)
            return n;
    }
    return 0;
}

char *stats_json(stats_shm_t *s, size_t *len)
{
    sb_t b = {.p = malloc(8192), .len = 0, .cap = 8192};
    if (!b.p)
        return NULL;
    uint64_t tot_req = 0, acc = 0, tls = 0, uc = 0, ur = 0, retr = 0, drop = 0, bufs = 0;
    int64_t active = 0;
    uint64_t resp[ST_NCLASS] = {0};

    agg_t *agg = calloc(STATS_MAX_SERVERS, sizeof(agg_t));
    stats_server_t *tmp = malloc(STATS_MAX_SERVERS * sizeof(stats_server_t));
    int nagg = 0, alive = 0;

    for (int i = 0; i < s->nworkers; i++) {
        stats_worker_t *w = &s->w[i];
        if (atomic_load(&w->pid) > 0)
            alive++;
        tot_req += atomic_load(&w->requests);
        acc += atomic_load(&w->accepted);
        active += atomic_load(&w->active_conns);
        tls += atomic_load(&w->tls_handshakes);
        uc += atomic_load(&w->upstream_connects);
        ur += atomic_load(&w->upstream_reuses);
        retr += atomic_load(&w->upstream_retries);
        drop += atomic_load(&w->log_dropped);
        bufs += atomic_load(&w->buffers_in_use);
        for (int c = 0; c < ST_NCLASS; c++)
            resp[c] += atomic_load(&w->resp[c]);
        if (!agg || !tmp || atomic_load(&w->pid) <= 0)
            continue;
        int n = snapshot(w, tmp);
        for (int k = 0; k < n; k++) {
            int j;
            for (j = 0; j < nagg; j++)
                if (strcmp(agg[j].s.backend, tmp[k].backend) == 0 &&
                    strcmp(agg[j].s.server, tmp[k].server) == 0)
                    break;
            if (j == nagg) {
                if (nagg == STATS_MAX_SERVERS)
                    continue;
                memset(&agg[j], 0, sizeof(agg[j]));
                str_copy(agg[j].s.backend, tmp[k].backend, sizeof(agg[j].s.backend));
                str_copy(agg[j].s.server, tmp[k].server, sizeof(agg[j].s.server));
                nagg++;
            }
            agg_t *a = &agg[j];
            a->seen_workers++;
            a->up_workers += tmp[k].up ? 1 : 0;
            a->s.active += tmp[k].active;
            a->s.idle += tmp[k].idle;
            a->s.requests += tmp[k].requests;
            a->s.failures += tmp[k].failures;
            if (tmp[k].load_known) {
                a->load_sum += tmp[k].load;
                a->load_n++;
            }
        }
    }

    uint64_t up_ms = mono_ms() - s->start_mono_ms;
    sb_printf(&b,
              "{\"uptime_s\":%.1f,\"workers\":%d,\"workers_alive\":%d,"
              "\"worker_restarts\":%llu,\"reloads_ok\":%llu,\"reloads_failed\":%llu,"
              "\"connections_accepted\":%llu,\"connections_active\":%lld,"
              "\"tls_handshakes\":%llu,\"requests_total\":%llu,"
              "\"responses\":{\"1xx\":%llu,\"2xx\":%llu,\"3xx\":%llu,\"4xx\":%llu,\"5xx\":%llu},"
              "\"upstream\":{\"connects\":%llu,\"reuses\":%llu,\"retries\":%llu},"
              "\"buffers_in_use\":%llu,\"log_dropped\":%llu,\"backends\":[",
              (double)up_ms / 1000.0, s->nworkers, alive,
              (unsigned long long)atomic_load(&s->worker_restarts),
              (unsigned long long)atomic_load(&s->reloads_ok),
              (unsigned long long)atomic_load(&s->reloads_failed),
              (unsigned long long)acc, (long long)active, (unsigned long long)tls,
              (unsigned long long)tot_req, (unsigned long long)resp[ST_1XX],
              (unsigned long long)resp[ST_2XX], (unsigned long long)resp[ST_3XX],
              (unsigned long long)resp[ST_4XX], (unsigned long long)resp[ST_5XX],
              (unsigned long long)uc, (unsigned long long)ur, (unsigned long long)retr,
              (unsigned long long)bufs, (unsigned long long)drop);
    for (int j = 0; j < nagg; j++) {
        agg_t *a = &agg[j];
        const char *state = a->up_workers == a->seen_workers ? "up"
                            : a->up_workers == 0             ? "down"
                                                             : "degraded";
        sb_printf(&b,
                  "%s{\"backend\":\"%s\",\"server\":\"%s\",\"state\":\"%s\","
                  "\"up_workers\":%d,\"active\":%d,\"idle\":%d,\"load\":",
                  j ? "," : "", a->s.backend, a->s.server, state, a->up_workers,
                  a->s.active, a->s.idle);
        if (a->load_n)
            sb_printf(&b, "%.3f", a->load_sum / a->load_n);
        else
            sb_printf(&b, "null");
        sb_printf(&b, ",\"requests\":%llu,\"failures\":%llu}",
                  (unsigned long long)a->s.requests, (unsigned long long)a->s.failures);
    }
    sb_printf(&b, "]}\n");
    free(agg);
    free(tmp);
    if (len)
        *len = b.len;
    return b.p;
}
