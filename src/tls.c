#include "tls.h"
#include "util.h"

#include <openssl/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct tls_fe {
    int n;
    int def;
    SSL_CTX **ctx;
    char (*pattern)[CFG_HOST_MAX];
};

static int ex_pattern_idx = -1;

int tls_global_init(void)
{
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, NULL) != 1)
        return -1;
    if (ex_pattern_idx < 0)
        ex_pattern_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    return ex_pattern_idx >= 0 ? 0 : -1;
}

bool tls_pattern_match(const char *pattern, const char *host, size_t hlen)
{
    size_t plen = strlen(pattern);
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *suffix = pattern + 1; /* ".dom" */
        size_t slen = plen - 1;
        if (hlen <= slen || strncasecmp(host + hlen - slen, suffix, slen) != 0)
            return false;
        /* el comodín cubre exactamente una etiqueta */
        return memchr(host, '.', hlen - slen) == NULL;
    }
    return plen == hlen && strncasecmp(pattern, host, hlen) == 0;
}

static int choose(const tls_fe_t *t, const char *name)
{
    size_t len = strlen(name);
    for (int i = 0; i < t->n; i++)
        if (t->pattern[i][0] != '*' && tls_pattern_match(t->pattern[i], name, len))
            return i;
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < t->n; i++) {
        size_t pl = strlen(t->pattern[i]);
        if (t->pattern[i][0] == '*' && tls_pattern_match(t->pattern[i], name, len) &&
            pl > best_len) {
            best = i;
            best_len = pl;
        }
    }
    return best;
}

static int servername_cb(SSL *ssl, int *alert, void *arg)
{
    (void)alert;
    tls_fe_t *t = arg;
    const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    int idx = name ? choose(t, name) : -1;
    if (idx < 0)
        idx = t->def;
    if (SSL_get_SSL_CTX(ssl) != t->ctx[idx])
        SSL_set_SSL_CTX(ssl, t->ctx[idx]);
    SSL_set_ex_data(ssl, ex_pattern_idx, t->pattern[idx]);
    return SSL_TLSEXT_ERR_OK;
}

static void ssl_err(char *err, size_t errlen, const char *what, const char *path)
{
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    snprintf(err, errlen, "%s '%s': %s", what, path, buf);
    ERR_clear_error();
}

static SSL_CTX *make_ctx(const cfg_cert_t *c, char *err, size_t errlen)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        ssl_err(err, errlen, "SSL_CTX_new", "");
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION |
                                 SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                              SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    static const unsigned char sid[] = "proxy";
    SSL_CTX_set_session_id_context(ctx, sid, sizeof(sid) - 1);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
    if (SSL_CTX_use_certificate_chain_file(ctx, c->cert) != 1) {
        ssl_err(err, errlen, "no se puede cargar el certificado", c->cert);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, c->key, SSL_FILETYPE_PEM) != 1) {
        ssl_err(err, errlen, "no se puede cargar la clave", c->key);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        ssl_err(err, errlen, "la clave no corresponde al certificado", c->cert);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

tls_fe_t *tls_fe_new(const cfg_frontend_t *fe, char *err, size_t errlen)
{
    tls_fe_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->n = fe->ncerts;
    t->def = fe->default_cert;
    t->ctx = calloc((size_t)t->n, sizeof(SSL_CTX *));
    t->pattern = calloc((size_t)t->n, sizeof(*t->pattern));
    if (!t->ctx || !t->pattern) {
        tls_fe_free(t);
        return NULL;
    }
    for (int i = 0; i < t->n; i++) {
        str_copy(t->pattern[i], fe->certs[i].sni, sizeof(t->pattern[i]));
        char e2[400];
        t->ctx[i] = make_ctx(&fe->certs[i], e2, sizeof(e2));
        if (!t->ctx[i]) {
            snprintf(err, errlen, "frontend '%s': %s", fe->name, e2);
            tls_fe_free(t);
            return NULL;
        }
        SSL_CTX_set_tlsext_servername_callback(t->ctx[i], servername_cb);
        SSL_CTX_set_tlsext_servername_arg(t->ctx[i], t);
    }
    return t;
}

void tls_fe_free(tls_fe_t *t)
{
    if (!t)
        return;
    if (t->ctx)
        for (int i = 0; i < t->n; i++)
            SSL_CTX_free(t->ctx[i]); /* refcount: los SSL vivos lo mantienen */
    free(t->ctx);
    free(t->pattern);
    free(t);
}

SSL *tls_fe_new_ssl(tls_fe_t *t, int fd)
{
    SSL *ssl = SSL_new(t->ctx[t->def]);
    if (!ssl)
        return NULL;
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    SSL_set_ex_data(ssl, ex_pattern_idx, t->pattern[t->def]);
    return ssl;
}

const char *tls_served_pattern(SSL *ssl)
{
    return SSL_get_ex_data(ssl, ex_pattern_idx);
}

int tls_validate_config(const config_t *c, char *err, size_t errlen)
{
    for (int i = 0; i < c->nfrontends; i++) {
        if (!c->frontends[i].tls)
            continue;
        tls_fe_t *t = tls_fe_new(&c->frontends[i], err, errlen);
        if (!t)
            return -1;
        tls_fe_free(t);
    }
    return 0;
}
