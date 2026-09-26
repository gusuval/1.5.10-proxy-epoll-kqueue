#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

/*
 * Parser HTTP/1.x incremental.
 *
 * La cabecera se parsea cuando está completa (\r\n\r\n); mientras tanto se
 * guarda la posición ya escaneada para no volver a recorrer bytes, así que
 * tolera cualquier fragmentación. Los campos apuntan dentro del buffer del
 * llamante (sin copias).
 *
 * El cuerpo se delimita con http_body_t (Content-Length, chunked o cierre),
 * que solo sigue los límites: los bytes se reenvían tal cual.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define HTTP_MAX_HEADERS 100

enum {
    HTTP_COMPLETE = 0,
    HTTP_INCOMPLETE = -1,
    HTTP_ERROR = -2,
    HTTP_TOO_LARGE = -3,
};

typedef struct {
    const char *name;
    const char *value;
    uint16_t nlen, vlen;
} http_header_t;

typedef struct {
    /* petición */
    const char *method;
    const char *target;
    uint16_t method_len, target_len;
    /* respuesta */
    int status;
    const char *reason;
    uint16_t reason_len;

    int version_minor; /* 0 o 1 */
    size_t head_len;   /* bytes de la cabecera, incluido \r\n\r\n */

    http_header_t headers[HTTP_MAX_HEADERS];
    int nheaders;

    /* derivados */
    const char *host; /* sin puerto */
    uint16_t host_len;
    bool has_host;
    int64_t content_length; /* -1 si no hay */
    bool chunked;
    bool has_te;
    bool conn_close, conn_keepalive, conn_upgrade;
    const char *upgrade;
    uint16_t upgrade_len;
    const char *backend_load; /* X-Backend-Load (respuestas) */
    uint16_t backend_load_len;
} http_msg_t;

/*
 * scan: estado entre llamadas (inicializar a 0 y pasar siempre el mismo
 * puntero mientras el buffer crece). Devuelve HTTP_COMPLETE,
 * HTTP_INCOMPLETE, HTTP_ERROR o HTTP_TOO_LARGE (> max_head sin terminar).
 */
int http_parse_request(http_msg_t *m, const char *buf, size_t len,
                       size_t max_head, size_t *scan);
int http_parse_response(http_msg_t *m, const char *buf, size_t len,
                        size_t max_head, size_t *scan);

/* ¿Es una cabecera hop-by-hop que no debe reenviarse? Incluye las
 * nombradas en Connection:, salvo Host/Content-Length/Transfer-Encoding. */
bool http_is_hop_by_hop(const http_msg_t *m, const char *name, size_t nlen);
bool http_conn_has_token(const http_msg_t *m, const char *tok, size_t len);
const http_header_t *http_find(const http_msg_t *m, const char *name);

typedef enum {
    BODY_NONE = 0,
    BODY_LENGTH,
    BODY_CHUNKED,
    BODY_EOF, /* delimitado por cierre (solo respuestas) */
} body_mode_t;

typedef struct {
    body_mode_t mode;
    uint64_t remaining; /* LENGTH: bytes restantes; CHUNKED: del chunk actual */
    int cstate;
    int ndigits;
    bool done;
} http_body_t;

void http_body_init(http_body_t *b, body_mode_t mode, uint64_t len);
/*
 * Consume hasta n bytes del cuerpo. Devuelve cuántos pertenecen al cuerpo
 * (el resto es el mensaje siguiente) o -1 si el chunked es inválido.
 * b->done indica fin del mensaje.
 */
ssize_t http_body_feed(http_body_t *b, const char *p, size_t n);

const char *http_reason(int status);

#endif
