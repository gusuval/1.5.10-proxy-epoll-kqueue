#include "config.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "toml.h"

static const char *strategy_names[] = {"round_robin", "weighted", "least_conn",
                                       "least_load"};

const char *strategy_name(strategy_t s) { return strategy_names[s]; }

typedef struct {
    char *err;
    size_t errlen;
    const char *base_dir;
} ctx_t;

static bool fail(ctx_t *x, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool fail(ctx_t *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->err, x->errlen, fmt, ap);
    va_end(ap);
    return false;
}

/* Rechaza claves desconocidas para detectar erratas en la config. */
static bool check_keys(ctx_t *x, toml_table_t *t, const char *where,
                       const char *const *allowed)
{
    for (int i = 0;; i++) {
        const char *k = toml_key_in(t, i);
        if (!k)
            break;
        bool ok = false;
        for (const char *const *a = allowed; *a; a++)
            if (strcmp(k, *a) == 0)
                ok = true;
        if (!ok)
            return fail(x, "%s: clave desconocida '%s'", where, k);
    }
    return true;
}

static bool get_str(ctx_t *x, toml_table_t *t, const char *key, char *out,
                    size_t outlen, bool required, const char *where)
{
    toml_datum_t d = toml_string_in(t, key);
    if (!d.ok) {
        if (toml_raw_in(t, key) || toml_array_in(t, key) || toml_table_in(t, key))
            return fail(x, "%s: '%s' debe ser una cadena", where, key);
        if (required)
            return fail(x, "%s: falta '%s'", where, key);
        return true;
    }
    size_t n = strlen(d.u.s);
    if (n >= outlen) {
        free(d.u.s);
        return fail(x, "%s: '%s' demasiado largo", where, key);
    }
    memcpy(out, d.u.s, n + 1);
    free(d.u.s);
    return true;
}

static bool get_int(ctx_t *x, toml_table_t *t, const char *key, int *out,
                    int minv, int maxv, const char *where)
{
    toml_datum_t d = toml_int_in(t, key);
    if (!d.ok) {
        if (toml_raw_in(t, key))
            return fail(x, "%s: '%s' debe ser un entero", where, key);
        return true; /* se mantiene el valor por defecto */
    }
    if (d.u.i < minv || d.u.i > maxv)
        return fail(x, "%s: '%s' fuera de rango [%d, %d]", where, key, minv,
                    maxv);
    *out = (int)d.u.i;
    return true;
}

static bool get_bool(ctx_t *x, toml_table_t *t, const char *key, bool *out,
                     const char *where)
{
    toml_datum_t d = toml_bool_in(t, key);
    if (!d.ok) {
        if (toml_raw_in(t, key))
            return fail(x, "%s: '%s' debe ser booleano", where, key);
        return true;
    }
    *out = d.u.b;
    return true;
}

static void resolve_path(ctx_t *x, char *p, size_t len)
{
    if (!p[0] || p[0] == '/' || strcmp(p, "-") == 0 || !x->base_dir)
        return;
    char tmp[CFG_PATH_MAX * 2];
    snprintf(tmp, sizeof(tmp), "%s/%s", x->base_dir, p);
    str_copy(p, tmp, len);
}

static bool valid_host_pattern(const char *h)
{
    if (strcmp(h, "default") == 0)
        return true;
    if (h[0] == '*') {
        if (h[1] != '.' || h[2] == '\0')
            return false;
        h += 2;
    }
    if (!*h)
        return false;
    for (; *h; h++) {
        char c = *h;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
            return false;
    }
    return true;
}

static bool parse_global(ctx_t *x, toml_table_t *root, config_t *c)
{
    toml_table_t *g = toml_table_in(root, "global");
    if (!g)
        return true;
    static const char *const keys[] = {"workers", "stats_socket", "log_file",
                                       "log_level", "access_log", NULL};
    if (!check_keys(x, g, "[global]", keys))
        return false;

    toml_datum_t w = toml_int_in(g, "workers");
    if (w.ok) {
        if (w.u.i < 1 || w.u.i > 128)
            return fail(x, "[global]: 'workers' fuera de rango [1, 128]");
        c->workers = (int)w.u.i;
    } else {
        char ws[16] = "";
        if (!get_str(x, g, "workers", ws, sizeof(ws), false, "[global]"))
            return false;
        if (ws[0] && strcmp(ws, "auto") != 0)
            return fail(x, "[global]: 'workers' debe ser \"auto\" o un entero");
    }
    if (!get_str(x, g, "stats_socket", c->stats_socket, sizeof(c->stats_socket),
                 false, "[global]") ||
        !get_str(x, g, "log_file", c->log_file, sizeof(c->log_file), false,
                 "[global]") ||
        !get_bool(x, g, "access_log", &c->access_log, "[global]"))
        return false;
    resolve_path(x, c->log_file, sizeof(c->log_file));

    char lvl[16] = "";
    if (!get_str(x, g, "log_level", lvl, sizeof(lvl), false, "[global]"))
        return false;
    if (lvl[0]) {
        int l = log_level_from_str(lvl);
        if (l < 0)
            return fail(x, "[global]: log_level '%s' inválido", lvl);
        c->log_level = l;
    }
    return true;
}

static bool parse_timeouts(ctx_t *x, toml_table_t *root, config_t *c)
{
    toml_table_t *t = toml_table_in(root, "timeouts");
    if (!t)
        return true;
    static const char *const keys[] = {"client_header", "client_idle",
                                       "upstream_connect", "upstream_read",
                                       "upstream_idle", NULL};
    const int mx = 3600 * 1000;
    return check_keys(x, t, "[timeouts]", keys) &&
           get_int(x, t, "client_header", &c->timeouts.client_header, 1, mx, "[timeouts]") &&
           get_int(x, t, "client_idle", &c->timeouts.client_idle, 1, mx, "[timeouts]") &&
           get_int(x, t, "upstream_connect", &c->timeouts.upstream_connect, 1, mx, "[timeouts]") &&
           get_int(x, t, "upstream_read", &c->timeouts.upstream_read, 1, mx, "[timeouts]") &&
           get_int(x, t, "upstream_idle", &c->timeouts.upstream_idle, 1, mx, "[timeouts]");
}

static bool parse_frontend(ctx_t *x, toml_table_t *t, int idx, cfg_frontend_t *f)
{
    char where[96];
    snprintf(where, sizeof(where), "[[frontend]] #%d", idx + 1);
    static const char *const keys[] = {"name", "listen", "tls", "certs",
                                       "default_cert", "route", NULL};
    if (!check_keys(x, t, where, keys) ||
        !get_str(x, t, "name", f->name, sizeof(f->name), true, where))
        return false;
    snprintf(where, sizeof(where), "frontend '%s'", f->name);
    if (!get_str(x, t, "listen", f->listen, sizeof(f->listen), true, where) ||
        !get_bool(x, t, "tls", &f->tls, where) ||
        !get_int(x, t, "default_cert", &f->default_cert, 0, 1000, where))
        return false;

    struct sockaddr_storage sa;
    socklen_t salen;
    char aerr[256];
    if (addr_parse(f->listen, true, &sa, &salen, aerr, sizeof(aerr)) < 0)
        return fail(x, "%s: listen: %s", where, aerr);
    addr_format((struct sockaddr *)&sa, f->key, sizeof(f->key));

    toml_array_t *certs = toml_array_in(t, "certs");
    if (certs) {
        int n = toml_array_nelem(certs);
        f->certs = calloc((size_t)(n ? n : 1), sizeof(cfg_cert_t));
        if (!f->certs)
            return fail(x, "sin memoria");
        for (int i = 0; i < n; i++) {
            toml_table_t *ct = toml_table_at(certs, i);
            char cw[128];
            snprintf(cw, sizeof(cw), "%s: certs[%d]", where, i);
            if (!ct)
                return fail(x, "%s: debe ser una tabla", cw);
            static const char *const ckeys[] = {"sni", "cert", "key", NULL};
            cfg_cert_t *cc = &f->certs[i];
            if (!check_keys(x, ct, cw, ckeys) ||
                !get_str(x, ct, "sni", cc->sni, sizeof(cc->sni), true, cw) ||
                !get_str(x, ct, "cert", cc->cert, sizeof(cc->cert), true, cw) ||
                !get_str(x, ct, "key", cc->key, sizeof(cc->key), true, cw))
                return false;
            if (strcmp(cc->sni, "default") == 0 || !valid_host_pattern(cc->sni))
                return fail(x, "%s: sni '%s' inválido", cw, cc->sni);
            str_lower(cc->sni);
            resolve_path(x, cc->cert, sizeof(cc->cert));
            resolve_path(x, cc->key, sizeof(cc->key));
            f->ncerts++;
        }
    }
    if (f->tls && f->ncerts == 0)
        return fail(x, "%s: tls = true requiere al menos un certificado", where);
    if (!f->tls && f->ncerts > 0)
        return fail(x, "%s: 'certs' definido pero tls = false", where);
    if (f->tls && f->default_cert >= f->ncerts)
        return fail(x, "%s: default_cert %d fuera de rango", where, f->default_cert);

    toml_array_t *routes = toml_array_in(t, "route");
    if (!routes)
        return fail(x, "%s: sin rutas ([[frontend.route]])", where);
    int n = toml_array_nelem(routes);
    f->routes = calloc((size_t)(n ? n : 1), sizeof(cfg_route_t));
    if (!f->routes)
        return fail(x, "sin memoria");
    int ndefault = 0;
    for (int i = 0; i < n; i++) {
        toml_table_t *rt = toml_table_at(routes, i);
        char rw[128];
        snprintf(rw, sizeof(rw), "%s: route[%d]", where, i);
        if (!rt)
            return fail(x, "%s: debe ser una tabla", rw);
        static const char *const rkeys[] = {"host", "backend", NULL};
        cfg_route_t *r = &f->routes[i];
        if (!check_keys(x, rt, rw, rkeys) ||
            !get_str(x, rt, "host", r->host, sizeof(r->host), true, rw) ||
            !get_str(x, rt, "backend", r->backend, sizeof(r->backend), true, rw))
            return false;
        if (!valid_host_pattern(r->host))
            return fail(x, "%s: host '%s' inválido", rw, r->host);
        str_lower(r->host);
        if (strcmp(r->host, "default") == 0)
            ndefault++;
        for (int j = 0; j < i; j++)
            if (strcmp(f->routes[j].host, r->host) == 0)
                return fail(x, "%s: host '%s' duplicado", where, r->host);
        f->nroutes++;
    }
    if (ndefault > 1)
        return fail(x, "%s: más de una ruta 'default'", where);
    if (f->nroutes == 0)
        return fail(x, "%s: sin rutas", where);
    return true;
}

static bool parse_backend(ctx_t *x, toml_table_t *t, int idx, cfg_backend_t *b)
{
    char where[96];
    snprintf(where, sizeof(where), "[[backend]] #%d", idx + 1);
    static const char *const keys[] = {"name", "strategy", "max_idle",
                                       "servers", "health", NULL};
    if (!check_keys(x, t, where, keys) ||
        !get_str(x, t, "name", b->name, sizeof(b->name), true, where))
        return false;
    snprintf(where, sizeof(where), "backend '%s'", b->name);

    char strat[32] = "round_robin";
    if (!get_str(x, t, "strategy", strat, sizeof(strat), false, where))
        return false;
    bool found = false;
    for (int i = 0; i < (int)ARRAY_LEN(strategy_names); i++)
        if (strcmp(strat, strategy_names[i]) == 0) {
            b->strategy = (strategy_t)i;
            found = true;
        }
    if (!found)
        return fail(x, "%s: strategy '%s' desconocida", where, strat);
    b->max_idle = 64;
    if (!get_int(x, t, "max_idle", &b->max_idle, 0, 100000, where))
        return false;

    toml_array_t *servers = toml_array_in(t, "servers");
    int n = servers ? toml_array_nelem(servers) : 0;
    if (n == 0)
        return fail(x, "%s: necesita al menos un servidor", where);
    if (n > CFG_MAX_SERVERS)
        return fail(x, "%s: máximo %d servidores", where, CFG_MAX_SERVERS);
    b->servers = calloc((size_t)n, sizeof(cfg_server_t));
    if (!b->servers)
        return fail(x, "sin memoria");
    for (int i = 0; i < n; i++) {
        toml_table_t *st = toml_table_at(servers, i);
        char sw[128];
        snprintf(sw, sizeof(sw), "%s: servers[%d]", where, i);
        if (!st)
            return fail(x, "%s: debe ser una tabla", sw);
        static const char *const skeys[] = {"addr", "weight", NULL};
        cfg_server_t *s = &b->servers[i];
        s->weight = 1;
        if (!check_keys(x, st, sw, skeys) ||
            !get_str(x, st, "addr", s->addr, sizeof(s->addr), true, sw) ||
            !get_int(x, st, "weight", &s->weight, 1, 1000, sw))
            return false;
        struct sockaddr_storage sa;
        socklen_t salen;
        char aerr[256];
        if (addr_parse(s->addr, true, &sa, &salen, aerr, sizeof(aerr)) < 0)
            return fail(x, "%s: %s", sw, aerr);
        b->nservers++;
    }

    cfg_health_t *h = &b->health;
    h->type = HEALTH_NONE;
    str_copy(h->path, "/health", sizeof(h->path));
    h->interval_ms = 2000;
    h->timeout_ms = 500;
    h->rise = 2;
    h->fall = 3;
    toml_table_t *ht = toml_table_in(t, "health");
    if (ht) {
        static const char *const hkeys[] = {"type", "path", "interval",
                                            "timeout", "rise", "fall", NULL};
        char hw[128];
        snprintf(hw, sizeof(hw), "%s: health", where);
        char type[16] = "tcp";
        if (!check_keys(x, ht, hw, hkeys) ||
            !get_str(x, ht, "type", type, sizeof(type), false, hw) ||
            !get_str(x, ht, "path", h->path, sizeof(h->path), false, hw) ||
            !get_int(x, ht, "interval", &h->interval_ms, 100, 3600000, hw) ||
            !get_int(x, ht, "timeout", &h->timeout_ms, 10, 60000, hw) ||
            !get_int(x, ht, "rise", &h->rise, 1, 100, hw) ||
            !get_int(x, ht, "fall", &h->fall, 1, 100, hw))
            return false;
        if (strcmp(type, "tcp") == 0)
            h->type = HEALTH_TCP;
        else if (strcmp(type, "http") == 0)
            h->type = HEALTH_HTTP;
        else if (strcmp(type, "none") == 0)
            h->type = HEALTH_NONE;
        else
            return fail(x, "%s: type '%s' desconocido", hw, type);
        if (h->path[0] != '/')
            return fail(x, "%s: path debe empezar por '/'", hw);
    }
    return true;
}

int config_find_backend(const config_t *c, const char *name)
{
    for (int i = 0; i < c->nbackends; i++)
        if (strcmp(c->backends[i].name, name) == 0)
            return i;
    return -1;
}

static bool validate(ctx_t *x, config_t *c)
{
    if (c->nfrontends == 0)
        return fail(x, "no hay ningún [[frontend]]");
    if (c->nbackends == 0)
        return fail(x, "no hay ningún [[backend]]");
    for (int i = 0; i < c->nfrontends; i++) {
        for (int j = 0; j < i; j++) {
            if (strcmp(c->frontends[i].name, c->frontends[j].name) == 0)
                return fail(x, "frontend '%s' duplicado", c->frontends[i].name);
            if (strcmp(c->frontends[i].key, c->frontends[j].key) == 0)
                return fail(x, "frontends '%s' y '%s' escuchan en el mismo puerto (%s)",
                            c->frontends[j].name, c->frontends[i].name,
                            c->frontends[i].key);
        }
        for (int r = 0; r < c->frontends[i].nroutes; r++) {
            const cfg_route_t *rt = &c->frontends[i].routes[r];
            if (config_find_backend(c, rt->backend) < 0)
                return fail(x, "frontend '%s': ruta '%s' apunta al backend inexistente '%s'",
                            c->frontends[i].name, rt->host, rt->backend);
        }
    }
    for (int i = 0; i < c->nbackends; i++)
        for (int j = 0; j < i; j++)
            if (strcmp(c->backends[i].name, c->backends[j].name) == 0)
                return fail(x, "backend '%s' duplicado", c->backends[i].name);
    return true;
}

config_t *config_parse(const char *text, const char *base_dir, char *err,
                       size_t errlen)
{
    ctx_t x = {.err = err, .errlen = errlen, .base_dir = base_dir};
    char terr[256];
    char *copy = strdup(text);
    if (!copy) {
        fail(&x, "sin memoria");
        return NULL;
    }
    toml_table_t *root = toml_parse(copy, terr, sizeof(terr));
    free(copy);
    if (!root) {
        fail(&x, "TOML inválido: %s", terr);
        return NULL;
    }

    config_t *c = calloc(1, sizeof(*c));
    if (!c) {
        toml_free(root);
        fail(&x, "sin memoria");
        return NULL;
    }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    c->workers = (int)(ncpu > 0 ? MIN(ncpu, 128) : 1);
    str_copy(c->stats_socket, "/tmp/proxy.sock", sizeof(c->stats_socket));
    c->log_level = LOG_INFO;
    c->access_log = true;
    c->timeouts = (cfg_timeouts_t){.client_header = 10000,
                                   .client_idle = 60000,
                                   .upstream_connect = 2000,
                                   .upstream_read = 30000,
                                   .upstream_idle = 30000};

    static const char *const keys[] = {"global", "timeouts", "frontend",
                                       "backend", NULL};
    bool ok = check_keys(&x, root, "raíz", keys) && parse_global(&x, root, c) &&
              parse_timeouts(&x, root, c);

    toml_array_t *fes = ok ? toml_array_in(root, "frontend") : NULL;
    if (ok && fes) {
        int n = toml_array_nelem(fes);
        c->frontends = calloc((size_t)(n ? n : 1), sizeof(cfg_frontend_t));
        for (int i = 0; ok && i < n; i++) {
            toml_table_t *t = toml_table_at(fes, i);
            ok = t ? parse_frontend(&x, t, i, &c->frontends[i])
                   : fail(&x, "[[frontend]] #%d inválido", i + 1);
            c->nfrontends++;
        }
    }
    toml_array_t *bes = ok ? toml_array_in(root, "backend") : NULL;
    if (ok && bes) {
        int n = toml_array_nelem(bes);
        c->backends = calloc((size_t)(n ? n : 1), sizeof(cfg_backend_t));
        for (int i = 0; ok && i < n; i++) {
            toml_table_t *t = toml_table_at(bes, i);
            ok = t ? parse_backend(&x, t, i, &c->backends[i])
                   : fail(&x, "[[backend]] #%d inválido", i + 1);
            c->nbackends++;
        }
    }
    if (ok)
        ok = validate(&x, c);
    toml_free(root);
    if (!ok) {
        config_free(c);
        return NULL;
    }
    return c;
}

config_t *config_load(const char *path, char *err, size_t errlen)
{
    char *text = read_file(path, NULL);
    if (!text) {
        snprintf(err, errlen, "no se puede leer '%s': %s", path, strerror(errno));
        return NULL;
    }
    char dir[CFG_PATH_MAX];
    str_copy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        str_copy(dir, ".", sizeof(dir));
    config_t *c = config_parse(text, dir, err, errlen);
    free(text);
    return c;
}

void config_free(config_t *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->nfrontends; i++) {
        free(c->frontends[i].certs);
        free(c->frontends[i].routes);
    }
    for (int i = 0; i < c->nbackends; i++)
        free(c->backends[i].servers);
    free(c->frontends);
    free(c->backends);
    free(c);
}
