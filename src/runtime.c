#include "runtime.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

runtime_t *runtime_build(config_t *cfg, char *err, size_t errlen)
{
    runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) {
        config_free(cfg);
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    rt->cfg = cfg;
    rt->refcnt = 1;
    rt->bes = calloc((size_t)cfg->nbackends, sizeof(backend_t));
    rt->fes = calloc((size_t)cfg->nfrontends, sizeof(frontend_t));
    if (!rt->bes || !rt->fes) {
        snprintf(err, errlen, "sin memoria");
        goto fail;
    }
    for (int i = 0; i < cfg->nbackends; i++) {
        if (backend_init(&rt->bes[i], &cfg->backends[i], err, errlen) < 0)
            goto fail;
        rt->nbe++;
    }
    for (int i = 0; i < cfg->nfrontends; i++) {
        const cfg_frontend_t *cf = &cfg->frontends[i];
        frontend_t *f = &rt->fes[i];
        rt->nfe++;
        str_copy(f->name, cf->name, sizeof(f->name));
        str_copy(f->key, cf->key, sizeof(f->key));
        f->tls = cf->tls;
        f->router = router_new();
        if (!f->router) {
            snprintf(err, errlen, "sin memoria");
            goto fail;
        }
        for (int r = 0; r < cf->nroutes; r++)
            if (router_add(f->router, cf->routes[r].host,
                           config_find_backend(cfg, cf->routes[r].backend)) < 0) {
                snprintf(err, errlen, "sin memoria");
                goto fail;
            }
        router_build(f->router);
        if (f->tls) {
            f->tls_fe = tls_fe_new(cf, err, errlen);
            if (!f->tls_fe)
                goto fail;
        }
    }
    return rt;
fail:
    runtime_free(rt);
    return NULL;
}

void runtime_start_health(runtime_t *rt)
{
    rt->health = health_start(rt->bes, rt->nbe);
}

frontend_t *runtime_frontend(runtime_t *rt, const char *key)
{
    for (int i = 0; i < rt->nfe; i++)
        if (strcmp(rt->fes[i].key, key) == 0)
            return &rt->fes[i];
    return NULL;
}

void runtime_free(runtime_t *rt)
{
    if (!rt)
        return;
    health_free(rt->health);
    for (int i = 0; i < rt->nfe; i++) {
        router_free(rt->fes[i].router);
        tls_fe_free(rt->fes[i].tls_fe);
    }
    for (int i = 0; i < rt->nbe; i++)
        backend_fini(&rt->bes[i]);
    free(rt->fes);
    free(rt->bes);
    config_free(rt->cfg);
    free(rt);
}
