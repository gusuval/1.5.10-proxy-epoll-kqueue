#ifndef ROUTER_H
#define ROUTER_H

/*
 * Tabla de rutas de un frontend: dominio exacto (hash djb2, direccionamiento
 * abierto) -> wildcard "*.dom" (gana el sufijo más largo) -> "default".
 * Las búsquedas son insensibles a mayúsculas.
 */

#include <stddef.h>

typedef struct router router_t;

router_t *router_new(void);
void router_free(router_t *r);
/* pattern: "host", "*.dominio" o "default". Devuelve 0 o -1. */
int router_add(router_t *r, const char *pattern, int target);
/* Termina de construir (ordena wildcards). */
void router_build(router_t *r);
/* host sin puerto; devuelve target o -1. */
int router_lookup(const router_t *r, const char *host, size_t len);

unsigned long djb2(const char *s, size_t len);

#endif
