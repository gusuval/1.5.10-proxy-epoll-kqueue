#ifndef HEALTH_H
#define HEALTH_H

/*
 * Sondas activas (TCP connect o GET path esperando 2xx) en un hilo aparte,
 * uno por generación de configuración. El hilo solo toca los campos
 * atómicos de server_t. Al retirar una configuración se pide la parada
 * (health_stop) y se espera a health_done() antes de liberar los backends.
 */

#include <stdbool.h>

#include "backend_pool.h"

typedef struct health health_t;

/* NULL si ningún backend tiene health activo (no es un error). */
health_t *health_start(backend_t *bes, int nbe);
void health_stop(health_t *h);
bool health_done(health_t *h);
void health_free(health_t *h);

/* Sonda síncrona (usada también por los tests). */
bool health_probe(const server_t *s, const cfg_health_t *cfg);

#endif
