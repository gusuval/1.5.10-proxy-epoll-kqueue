#ifndef TLS_H
#define TLS_H

/*
 * Terminación TLS (OpenSSL >= 3.0). Cada frontend TLS tiene un SSL_CTX por
 * certificado; el callback de servername elige el certificado según el SNI
 * (exacto, luego wildcard de una etiqueta, luego default_cert).
 */

#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>

#include "config.h"

typedef struct tls_fe tls_fe_t;

int tls_global_init(void);
tls_fe_t *tls_fe_new(const cfg_frontend_t *fe, char *err, size_t errlen);
void tls_fe_free(tls_fe_t *t);
/* SSL nuevo en modo servidor asociado al fd. */
SSL *tls_fe_new_ssl(tls_fe_t *t, int fd);
/* Patrón (campo sni de la config) del certificado servido. */
const char *tls_served_pattern(SSL *ssl);

/* Casa un nombre con un patrón de certificado: exacto o "*.dom" de una
 * sola etiqueta (RFC 6125). */
bool tls_pattern_match(const char *pattern, const char *host, size_t hlen);

/* Valida que todos los frontends TLS cargan sus certificados. */
int tls_validate_config(const config_t *c, char *err, size_t errlen);

#endif
