#include "http_parser.h"
#include "util.h"

#include <ctype.h>
#include <string.h>

/* tchar de RFC 9110 */
static const unsigned char tchar[256] = {
    ['!'] = 1, ['#'] = 1, ['$'] = 1, ['%'] = 1, ['&'] = 1, ['\''] = 1,
    ['*'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1, ['^'] = 1, ['_'] = 1,
    ['`'] = 1, ['|'] = 1, ['~'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1,
    ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1,
    ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1,
    ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1,
    ['S'] = 1, ['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1,
    ['Y'] = 1, ['Z'] = 1,
    ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1,
    ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1,
    ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1,
    ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1,
    ['y'] = 1, ['z'] = 1,
};

/* Busca el final de cabecera. Devuelve longitud o 0 si no está. */
static size_t find_head_end(const char *buf, size_t len, size_t *scan)
{
    size_t i = *scan > 3 ? *scan - 3 : 0;
    for (; i + 3 < len; i++) {
        const char *p = memchr(buf + i, '\r', len - i - 3);
        if (!p)
            break;
        i = (size_t)(p - buf);
        if (p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return i + 4;
    }
    *scan = len;
    return 0;
}

static const char *trim(const char *s, const char *e, uint16_t *outlen)
{
    while (s < e && (*s == ' ' || *s == '\t'))
        s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *outlen = (uint16_t)(e - s);
    return s;
}

/* ¿La lista separada por comas contiene tok (case-insensitive)? */
static bool list_has(const char *v, size_t vlen, const char *tok, size_t tlen)
{
    const char *p = v, *end = v + vlen;
    while (p < end) {
        const char *c = memchr(p, ',', (size_t)(end - p));
        const char *te = c ? c : end;
        uint16_t l;
        const char *t = trim(p, te, &l);
        if (l == tlen && strncasecmp(t, tok, tlen) == 0)
            return true;
        p = te + 1;
    }
    return false;
}

/* Último elemento de la lista. */
static const char *list_last(const char *v, size_t vlen, uint16_t *outlen)
{
    const char *end = v + vlen, *p = end;
    while (p > v && p[-1] != ',')
        p--;
    return trim(p, end, outlen);
}

static int parse_headers(http_msg_t *m, const char *p, const char *end,
                         bool is_request)
{
    m->nheaders = 0;
    m->content_length = -1;
    m->chunked = m->has_te = m->has_host = false;
    m->conn_close = m->conn_keepalive = m->conn_upgrade = false;
    m->host = NULL;
    m->host_len = 0;
    m->upgrade = NULL;
    m->upgrade_len = 0;
    m->backend_load = NULL;
    m->backend_load_len = 0;

    while (p < end) {
        const char *eol = memchr(p, '\r', (size_t)(end - p));
        if (!eol || eol + 1 >= end || eol[1] != '\n')
            return HTTP_ERROR;
        if (eol == p) /* línea vacía: fin */
            break;
        if (*p == ' ' || *p == '\t') /* obs-fold */
            return HTTP_ERROR;
        const char *colon = p;
        while (colon < eol && tchar[(unsigned char)*colon])
            colon++;
        if (colon == p || colon >= eol || *colon != ':')
            return HTTP_ERROR; /* nombre vacío o espacio antes de ':' */
        for (const char *q = colon + 1; q < eol; q++) {
            unsigned char ch = (unsigned char)*q;
            if (ch == '\n' || ch == '\0' || (ch < 0x20 && ch != '\t') || ch == 0x7f)
                return HTTP_ERROR;
        }
        if (m->nheaders == HTTP_MAX_HEADERS)
            return HTTP_TOO_LARGE;
        http_header_t *h = &m->headers[m->nheaders++];
        h->name = p;
        h->nlen = (uint16_t)(colon - p);
        h->value = trim(colon + 1, eol, &h->vlen);

        if (str_ieq(h->name, h->nlen, "content-length")) {
            if (h->vlen == 0 || h->vlen > 18)
                return HTTP_ERROR;
            int64_t v = 0;
            for (uint16_t i = 0; i < h->vlen; i++) {
                if (!isdigit((unsigned char)h->value[i]))
                    return HTTP_ERROR;
                v = v * 10 + (h->value[i] - '0');
            }
            if (m->content_length >= 0 && m->content_length != v)
                return HTTP_ERROR; /* CL duplicados distintos */
            m->content_length = v;
        } else if (str_ieq(h->name, h->nlen, "transfer-encoding")) {
            m->has_te = true;
            uint16_t l;
            const char *last = list_last(h->value, h->vlen, &l);
            m->chunked = str_ieq(last, l, "chunked");
        } else if (str_ieq(h->name, h->nlen, "connection")) {
            if (list_has(h->value, h->vlen, "close", 5))
                m->conn_close = true;
            if (list_has(h->value, h->vlen, "keep-alive", 10))
                m->conn_keepalive = true;
            if (list_has(h->value, h->vlen, "upgrade", 7))
                m->conn_upgrade = true;
        } else if (str_ieq(h->name, h->nlen, "upgrade")) {
            m->upgrade = h->value;
            m->upgrade_len = h->vlen;
        } else if (is_request && str_ieq(h->name, h->nlen, "host")) {
            if (m->has_host)
                return HTTP_ERROR; /* Host duplicado */
            m->has_host = true;
            /* quitar el puerto: "host:port" o "[v6]:port" */
            const char *v = h->value, *ve = h->value + h->vlen;
            if (v < ve && *v == '[') {
                const char *rb = memchr(v, ']', (size_t)(ve - v));
                if (!rb)
                    return HTTP_ERROR;
                ve = rb + 1;
            } else {
                const char *c = memchr(v, ':', (size_t)(ve - v));
                if (c)
                    ve = c;
            }
            for (const char *q = v; q < ve; q++)
                if (*q == ' ' || *q == '/' || *q == '@')
                    return HTTP_ERROR;
            m->host = v;
            m->host_len = (uint16_t)(ve - v);
        } else if (!is_request && str_ieq(h->name, h->nlen, "x-backend-load")) {
            m->backend_load = h->value;
            m->backend_load_len = h->vlen;
        }
        p = eol + 2;
    }

    if (is_request) {
        /* Protección frente a request smuggling (RFC 9112 §6.3). */
        if (m->has_te && m->content_length >= 0)
            return HTTP_ERROR;
        if (m->has_te && !m->chunked)
            return HTTP_ERROR;
        if (m->version_minor == 1 && !m->has_host)
            return HTTP_ERROR;
    } else if (m->has_te) {
        m->content_length = -1; /* TE gana a CL en respuestas */
    }
    return HTTP_COMPLETE;
}

static int parse_version(const char *p, const char *end, int *minor)
{
    if (end - p != 8 || memcmp(p, "HTTP/1.", 7) != 0)
        return -1;
    if (p[7] == '0')
        *minor = 0;
    else if (p[7] == '1')
        *minor = 1;
    else
        return -1;
    return 0;
}

int http_parse_request(http_msg_t *m, const char *buf, size_t len,
                       size_t max_head, size_t *scan)
{
    /* RFC 9112 §2.2: ignorar CRLF iniciales */
    size_t skip = 0;
    while (skip + 1 < len && buf[skip] == '\r' && buf[skip + 1] == '\n')
        skip += 2;
    size_t hl = find_head_end(buf, len, scan);
    if (!hl)
        return len > max_head ? HTTP_TOO_LARGE : HTTP_INCOMPLETE;
    if (hl > max_head)
        return HTTP_TOO_LARGE;
    if (hl <= skip)
        return HTTP_ERROR;
    m->head_len = hl;
    const char *p = buf + skip, *end = buf + hl - 2; /* end apunta al \r\n final */

    const char *eol = memchr(p, '\r', (size_t)(end + 2 - p));
    if (!eol || eol[1] != '\n')
        return HTTP_ERROR;
    const char *sp1 = memchr(p, ' ', (size_t)(eol - p));
    if (!sp1 || sp1 == p)
        return HTTP_ERROR;
    for (const char *q = p; q < sp1; q++)
        if (!tchar[(unsigned char)*q])
            return HTTP_ERROR;
    const char *tgt = sp1 + 1;
    const char *sp2 = memchr(tgt, ' ', (size_t)(eol - tgt));
    if (!sp2 || sp2 == tgt)
        return HTTP_ERROR;
    for (const char *q = tgt; q < sp2; q++)
        if ((unsigned char)*q <= 0x20 || *q == 0x7f)
            return HTTP_ERROR;
    if (parse_version(sp2 + 1, eol, &m->version_minor) < 0)
        return HTTP_ERROR;
    if (sp1 - p > 32 || sp2 - tgt > 8192)
        return HTTP_TOO_LARGE;

    m->method = p;
    m->method_len = (uint16_t)(sp1 - p);
    m->target = tgt;
    m->target_len = (uint16_t)(sp2 - tgt);
    m->status = 0;
    m->reason = NULL;
    m->reason_len = 0;
    return parse_headers(m, eol + 2, end + 2, true);
}

int http_parse_response(http_msg_t *m, const char *buf, size_t len,
                        size_t max_head, size_t *scan)
{
    size_t hl = find_head_end(buf, len, scan);
    if (!hl)
        return len > max_head ? HTTP_TOO_LARGE : HTTP_INCOMPLETE;
    if (hl > max_head)
        return HTTP_TOO_LARGE;
    m->head_len = hl;
    const char *p = buf, *end = buf + hl - 2;
    const char *eol = memchr(p, '\r', (size_t)(end + 2 - p));
    if (!eol || eol[1] != '\n' || eol - p < 12)
        return HTTP_ERROR;
    if (parse_version(p, p + 8, &m->version_minor) < 0 || p[8] != ' ')
        return HTTP_ERROR;
    if (!isdigit((unsigned char)p[9]) || !isdigit((unsigned char)p[10]) ||
        !isdigit((unsigned char)p[11]))
        return HTTP_ERROR;
    m->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    if (m->status < 100)
        return HTTP_ERROR;
    if (eol > p + 12) {
        if (p[12] != ' ')
            return HTTP_ERROR;
        m->reason = p + 13;
        m->reason_len = (uint16_t)(eol - (p + 13));
    } else {
        m->reason = p + 12;
        m->reason_len = 0;
    }
    m->method = m->target = NULL;
    m->method_len = m->target_len = 0;
    return parse_headers(m, eol + 2, end + 2, false);
}

bool http_conn_has_token(const http_msg_t *m, const char *tok, size_t len)
{
    for (int i = 0; i < m->nheaders; i++) {
        const http_header_t *h = &m->headers[i];
        if (str_ieq(h->name, h->nlen, "connection") &&
            list_has(h->value, h->vlen, tok, len))
            return true;
    }
    return false;
}

bool http_is_hop_by_hop(const http_msg_t *m, const char *name, size_t nlen)
{
    static const char *const hop[] = {"connection", "keep-alive",
                                      "proxy-connection", "te", "trailer",
                                      "upgrade", "proxy-authenticate",
                                      "proxy-authorization"};
    for (size_t i = 0; i < ARRAY_LEN(hop); i++)
        if (str_ieq(name, nlen, hop[i]))
            return true;
    /* Nunca eliminar cabeceras que afectan al encuadre ni Host, aunque se
     * nombren en Connection (evita ataques de smuggling vía hop-by-hop). */
    if (str_ieq(name, nlen, "content-length") ||
        str_ieq(name, nlen, "transfer-encoding") || str_ieq(name, nlen, "host"))
        return false;
    return http_conn_has_token(m, name, nlen);
}

const http_header_t *http_find(const http_msg_t *m, const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < m->nheaders; i++)
        if (str_ieq(m->headers[i].name, m->headers[i].nlen, name) && n == m->headers[i].nlen)
            return &m->headers[i];
    return NULL;
}

/* ---------------- cuerpo ---------------- */

enum {
    CH_SIZE,       /* dígitos hex */
    CH_EXT,        /* extensión hasta \r */
    CH_SIZE_LF,    /* \n tras la línea de tamaño */
    CH_DATA,       /* datos del chunk */
    CH_DATA_CR,    /* \r tras los datos */
    CH_DATA_LF,    /* \n tras los datos */
    CH_TRAILER,    /* inicio de línea de trailer (o \r final) */
    CH_TRAILER_LN, /* dentro de una línea de trailer */
    CH_END_LF,     /* \n final */
};

void http_body_init(http_body_t *b, body_mode_t mode, uint64_t len)
{
    b->mode = mode;
    b->remaining = len;
    b->cstate = CH_SIZE;
    b->ndigits = 0;
    b->done = (mode == BODY_NONE) || (mode == BODY_LENGTH && len == 0);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

ssize_t http_body_feed(http_body_t *b, const char *p, size_t n)
{
    if (b->done)
        return 0;
    switch (b->mode) {
    case BODY_NONE:
        b->done = true;
        return 0;
    case BODY_EOF:
        return (ssize_t)n;
    case BODY_LENGTH: {
        size_t k = n < b->remaining ? n : (size_t)b->remaining;
        b->remaining -= k;
        if (b->remaining == 0)
            b->done = true;
        return (ssize_t)k;
    }
    case BODY_CHUNKED:
        break;
    }

    size_t i = 0;
    while (i < n && !b->done) {
        char c = p[i];
        switch (b->cstate) {
        case CH_SIZE: {
            int v = hexval(c);
            if (v >= 0) {
                if (++b->ndigits > 15)
                    return -1;
                b->remaining = (b->remaining << 4) | (uint64_t)v;
                i++;
            } else if (b->ndigits == 0) {
                return -1;
            } else if (c == ';' || c == ' ' || c == '\t') {
                b->cstate = CH_EXT;
                i++;
            } else if (c == '\r') {
                b->cstate = CH_SIZE_LF;
                i++;
            } else {
                return -1;
            }
            break;
        }
        case CH_EXT:
            if (c == '\r')
                b->cstate = CH_SIZE_LF;
            else if (c == '\n')
                return -1;
            i++;
            break;
        case CH_SIZE_LF:
            if (c != '\n')
                return -1;
            i++;
            b->cstate = b->remaining ? CH_DATA : CH_TRAILER;
            break;
        case CH_DATA: {
            size_t k = n - i < b->remaining ? n - i : (size_t)b->remaining;
            i += k;
            b->remaining -= k;
            if (!b->remaining)
                b->cstate = CH_DATA_CR;
            break;
        }
        case CH_DATA_CR:
            if (c != '\r')
                return -1;
            i++;
            b->cstate = CH_DATA_LF;
            break;
        case CH_DATA_LF:
            if (c != '\n')
                return -1;
            i++;
            b->cstate = CH_SIZE;
            b->ndigits = 0;
            b->remaining = 0;
            break;
        case CH_TRAILER:
            if (c == '\r') {
                b->cstate = CH_END_LF;
            } else if (c == '\n') {
                return -1;
            } else {
                b->cstate = CH_TRAILER_LN;
            }
            i++;
            break;
        case CH_TRAILER_LN:
            if (c == '\n')
                b->cstate = CH_TRAILER;
            i++;
            break;
        case CH_END_LF:
            if (c != '\n')
                return -1;
            i++;
            b->done = true;
            break;
        }
    }
    return (ssize_t)i;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 421: return "Misdirected Request";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Unknown";
    }
}
