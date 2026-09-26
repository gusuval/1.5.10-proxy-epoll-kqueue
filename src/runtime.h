#ifndef RUNTIME_H
#define RUNTIME_H

/*
 * Una generación de configuración ya "compilada": routers por frontend,
 * contextos TLS, backends con sus servidores y el hilo de health.
 *
 * El worker mantiene un puntero a la generación actual con una referencia
 * propia; cada petición toma una referencia mientras está en vuelo. Al
 * recargar se cambia el puntero (swap) y la vieja se libera cuando su
 * refcount llega a 0 y su hilo de health ha terminado (esquema RCU).
 * Todo esto ocurre en el hilo del event loop: el refcount no es atómico.
 */

#include <stdint.h>

#include "backend_pool.h"
#include "config.h"
#include "health.h"
#include "router.h"
#include "tls.h"

typedef struct frontend {
    char name[CFG_NAME_MAX];
    char key[CFG_ADDR_MAX];
    bool tls;
    router_t *router;
    tls_fe_t *tls_fe;
} frontend_t;

typedef struct runtime {
    config_t *cfg;
    frontend_t *fes;
    int nfe;
    backend_t *bes;
    int nbe;
    int refcnt;
    uint64_t gen;
    health_t *health;
    struct runtime *next; /* lista de retiradas */
} runtime_t;

/* Toma posesión de cfg (también en caso de error). */
runtime_t *runtime_build(config_t *cfg, char *err, size_t errlen);
void runtime_start_health(runtime_t *rt);
frontend_t *runtime_frontend(runtime_t *rt, const char *key);
void runtime_free(runtime_t *rt);

#endif
