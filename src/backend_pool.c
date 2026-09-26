#include "backend_pool.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int backend_init(backend_t *b, const cfg_backend_t *cfg, char *err, size_t errlen)
{
    memset(b, 0, sizeof(*b));
    str_copy(b->name, cfg->name, sizeof(b->name));
    b->strategy = cfg->strategy;
    b->max_idle = cfg->max_idle;
    b->health = cfg->health;
    b->rng = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)b ^ mono_ms();
    b->servers = calloc((size_t)cfg->nservers, sizeof(server_t));
    if (!b->servers)
        return -1;
    b->nservers = cfg->nservers;
    for (int i = 0; i < cfg->nservers; i++) {
        server_t *s = &b->servers[i];
        s->idx = i;
        s->weight = cfg->servers[i].weight;
        str_copy(s->addr_str, cfg->servers[i].addr, sizeof(s->addr_str));
        if (addr_parse(cfg->servers[i].addr, true, &s->sa, &s->salen, err, errlen) < 0)
            return -1;
        atomic_init(&s->up, 1);
        atomic_init(&s->fails, 0);
        atomic_init(&s->oks, 0);
    }
    return 0;
}

void backend_fini(backend_t *b)
{
    free(b->servers);
    b->servers = NULL;
    b->nservers = 0;
}

bool server_is_up(backend_t *b, server_t *s, uint64_t now)
{
    if (atomic_load_explicit(&s->up, memory_order_relaxed))
        return true;
    /* Sin health activo, un servidor caído se reintenta tras 'interval'
     * (half-open): si vuelve a fallar se marca de nuevo como caído. */
    if (b->health.type == HEALTH_NONE && now - s->down_since >= (uint64_t)b->health.interval_ms) {
        /* un solo fallo más lo vuelve a marcar como caído */
        atomic_store(&s->fails, b->health.fall - 1);
        atomic_store(&s->up, 1);
        return true;
    }
    return false;
}

static inline bool usable(backend_t *b, server_t *s, uint64_t exclude, uint64_t now)
{
    if (s->idx < 64 && (exclude & (1ULL << s->idx)))
        return false;
    return server_is_up(b, s, now);
}

static server_t *pick_rr(backend_t *b, uint64_t ex, uint64_t now)
{
    for (int k = 0; k < b->nservers; k++) {
        server_t *s = &b->servers[(b->rr + (unsigned)k) % (unsigned)b->nservers];
        if (usable(b, s, ex, now)) {
            b->rr = (b->rr + (unsigned)k + 1) % (unsigned)b->nservers;
            return s;
        }
    }
    return NULL;
}

/* Smooth weighted round robin (nginx): reparto proporcional sin ráfagas. */
static server_t *pick_weighted(backend_t *b, uint64_t ex, uint64_t now)
{
    server_t *best = NULL;
    int total = 0;
    for (int i = 0; i < b->nservers; i++) {
        server_t *s = &b->servers[i];
        if (!usable(b, s, ex, now))
            continue;
        s->cur_weight += s->weight;
        total += s->weight;
        if (!best || s->cur_weight > best->cur_weight)
            best = s;
    }
    if (best)
        best->cur_weight -= total;
    return best;
}

static server_t *pick_least_conn(backend_t *b, uint64_t ex, uint64_t now)
{
    server_t *best = NULL;
    for (int k = 0; k < b->nservers; k++) {
        server_t *s = &b->servers[(b->rr + (unsigned)k) % (unsigned)b->nservers];
        if (!usable(b, s, ex, now))
            continue;
        if (!best || s->active < best->active)
            best = s;
    }
    if (best)
        b->rr = (unsigned)(best->idx + 1) % (unsigned)b->nservers;
    return best;
}

static bool load_valid(const server_t *s, uint64_t now)
{
    return s->load_known && now - s->load_ts <= LOAD_STALE_MS;
}

/* Power of two choices sobre la carga reportada. */
static server_t *pick_least_load(backend_t *b, uint64_t ex, uint64_t now)
{
    server_t *cand[CFG_MAX_SERVERS];
    int n = 0;
    for (int i = 0; i < b->nservers; i++)
        if (usable(b, &b->servers[i], ex, now))
            cand[n++] = &b->servers[i];
    if (n == 0)
        return NULL;
    if (n == 1)
        return cand[0];
    int i = (int)(rng_next(&b->rng) % (uint64_t)n);
    int j = (int)(rng_next(&b->rng) % (uint64_t)(n - 1));
    if (j >= i)
        j++;
    server_t *x = cand[i], *y = cand[j];
    bool kx = load_valid(x, now), ky = load_valid(y, now);
    if (kx && ky && fabs(x->load - y->load) > 1e-9)
        return x->load < y->load ? x : y;
    /* Carga desconocida, caducada o empate: desempata least_conn. */
    if (x->active != y->active)
        return x->active < y->active ? x : y;
    /* Empate total: se prefiere al que no tiene carga fresca, para
     * conocerla cuanto antes (exploración). */
    if (kx != ky)
        return kx ? y : x;
    return x;
}

server_t *backend_pick(backend_t *b, uint64_t exclude, uint64_t now)
{
    if (b->nservers == 0)
        return NULL;
    switch (b->strategy) {
    case STRAT_WEIGHTED:
        return pick_weighted(b, exclude, now);
    case STRAT_LEAST_CONN:
        return pick_least_conn(b, exclude, now);
    case STRAT_LEAST_LOAD:
        return pick_least_load(b, exclude, now);
    case STRAT_ROUND_ROBIN:
    default:
        return pick_rr(b, exclude, now);
    }
}

void server_report_load(server_t *s, double v, uint64_t now)
{
    if (!load_valid(s, now))
        s->load = v;
    else
        s->load = LOAD_EMA_ALPHA * v + (1.0 - LOAD_EMA_ALPHA) * s->load;
    s->load_known = true;
    s->load_ts = now;
}

bool server_passive_fail(backend_t *b, server_t *s, uint64_t now)
{
    s->failures++;
    atomic_store(&s->oks, 0);
    int f = atomic_fetch_add(&s->fails, 1) + 1;
    if (f >= b->health.fall && atomic_exchange(&s->up, 0)) {
        s->down_since = now;
        return true;
    }
    return false;
}

void server_passive_ok(server_t *s)
{
    if (atomic_load_explicit(&s->fails, memory_order_relaxed))
        atomic_store(&s->fails, 0);
}

bool server_probe_result(backend_t *b, server_t *s, bool ok)
{
    if (ok) {
        atomic_store(&s->fails, 0);
        int k = atomic_fetch_add(&s->oks, 1) + 1;
        if (k >= b->health.rise && !atomic_load(&s->up)) {
            atomic_store(&s->up, 1);
            return true;
        }
        return false;
    }
    atomic_store(&s->oks, 0);
    int f = atomic_fetch_add(&s->fails, 1) + 1;
    if (f >= b->health.fall && atomic_exchange(&s->up, 0)) {
        s->down_since = mono_ms();
        return true;
    }
    return false;
}

bool parse_load(const char *v, size_t len, double *out)
{
    char tmp[32];
    while (len && (*v == ' ' || *v == '\t')) {
        v++;
        len--;
    }
    while (len && (v[len - 1] == ' ' || v[len - 1] == '\t'))
        len--;
    if (len == 0 || len >= sizeof(tmp))
        return false;
    memcpy(tmp, v, len);
    tmp[len] = '\0';
    char *end;
    double d = strtod(tmp, &end);
    if (*end != '\0' || !isfinite(d) || d < 0.0 || d > 1.0)
        return false;
    *out = d;
    return true;
}
