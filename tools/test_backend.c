/*
 * Backend HTTP/1.1 de pruebas sobre la abstracción io_event (epoll en
 * Linux, kqueue en macOS/BSD).
 *
 *   test_backend --port 9001 --name api-1 [--latency-ms N]
 *                [--load F | --load-auto] [--threads N] [--bind IP]
 *
 * Endpoints:
 *   /health          200 "ok"
 *   /echo            devuelve el cuerpo tal cual (Content-Length o chunked)
 *   /headers         devuelve la cabecera de la petición recibida
 *   /chunked         respuesta chunked en varios trozos
 *   /close           respuesta delimitada por cierre de conexión
 *   /nocontent       204
 *   /sleep/<ms>      responde tras <ms> milisegundos
 *   /big/<n>         cuerpo de n bytes
 *   /stats           JSON con conexiones y peticiones atendidas
 *   /ws              WebSocket eco
 *   cualquier otra   "hello from <name>" (~100 B)
 * Todas las respuestas llevan X-Backend y, con --load/--load-auto,
 * X-Backend-Load.
 *
 * --drop-every N: en cada conexión, la petición N, 2N... (nunca la primera)
 * se descarta cerrando la conexión sin responder. Simula un backend que
 * cierra conexiones keep-alive justo cuando el proxy las reutiliza.
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "http_parser.h"
#include "io_event.h"
#include "util.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static struct {
    int port;
    char bind_ip[64];
    char name[64];
    int latency_ms;
    double load;
    bool load_fixed, load_auto;
    int threads;
    int drop_every;
} opt = {.port = 9001, .bind_ip = "127.0.0.1", .name = "backend", .threads = 1};

static atomic_uint_fast64_t n_conns, n_requests;
static atomic_int n_active;

typedef struct {
    char *p;
    size_t len, cap, off; /* off: bytes ya consumidos (in) o enviados (out) */
} dbuf_t;

static bool db_reserve(dbuf_t *b, size_t extra)
{
    /* No se rebobina aquí: el cuerpo en curso se referencia por offset
     * desde body_start. Lo hace process() entre peticiones. */
    if (b->len + extra <= b->cap)
        return true;
    size_t nc = b->cap ? b->cap : 16384;
    while (nc < b->len + extra)
        nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np)
        return false;
    b->p = np;
    b->cap = nc;
    return true;
}

static void db_put(dbuf_t *b, const void *d, size_t n)
{
    if (!db_reserve(b, n))
        return;
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

static void db_printf(dbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void db_printf(dbuf_t *b, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        db_put(b, tmp, (size_t)MIN((size_t)n, sizeof(tmp) - 1));
}

enum { B_HEAD, B_BODY, B_WAIT, B_WS, B_CLOSING };

typedef struct {
    io_loop_t *loop;
} tctx_t;

typedef struct bconn {
    io_handler_t h;
    tctx_t *t;
    int fd;
    int st;
    bool rd, wr, eof, closed, keepalive, head_req;
    dbuf_t in, out;
    size_t scan;
    http_body_t body;
    size_t body_start;   /* offset en in donde empieza el cuerpo */
    size_t head_len;
    char path[256];
    char method[16];
    bool chunked_req;
    char *head_copy;     /* para /headers */
    size_t head_copy_len;
    char ws_key[64];
    bool is_ws;
    io_timer_t delay;
    dbuf_t pending; /* respuesta retenida por latencia */
    unsigned nreq;
} bconn_t;

static void conn_drive(bconn_t *c);

static void conn_free(void *p)
{
    bconn_t *c = p;
    free(c->in.p);
    free(c->out.p);
    free(c->pending.p);
    free(c->head_copy);
    free(c);
}

static void conn_close(bconn_t *c)
{
    if (c->closed)
        return;
    c->closed = true;
    io_timer_cancel(c->t->loop, &c->delay);
    io_loop_del(c->t->loop, c->fd);
    close(c->fd);
    atomic_fetch_sub(&n_active, 1);
    io_loop_defer(c->t->loop, conn_free, c);
}

static double current_load(void)
{
    if (opt.load_fixed)
        return opt.load;
    double l = atomic_load(&n_active) / 50.0;
    return l > 1.0 ? 1.0 : l;
}

/* Cabecera de respuesta común. body_len < 0 => chunked; -2 => sin longitud. */
static void resp_head(bconn_t *c, dbuf_t *o, int status, const char *reason,
                      const char *ctype, long long body_len)
{
    db_printf(o, "HTTP/1.1 %d %s\r\nX-Backend: %s\r\n", status, reason, opt.name);
    if (opt.load_fixed || opt.load_auto)
        db_printf(o, "X-Backend-Load: %.3f\r\n", current_load());
    if (ctype)
        db_printf(o, "Content-Type: %s\r\n", ctype);
    if (body_len >= 0)
        db_printf(o, "Content-Length: %lld\r\n", body_len);
    else if (body_len == -1)
        db_printf(o, "Transfer-Encoding: chunked\r\n");
    if (!c->keepalive)
        db_printf(o, "Connection: close\r\n");
    db_put(o, "\r\n", 2);
}

static void simple(bconn_t *c, dbuf_t *o, int status, const char *reason, const char *body)
{
    size_t n = strlen(body);
    resp_head(c, o, status, reason, "text/plain", (long long)n);
    if (!c->head_req)
        db_put(o, body, n);
}

static void ws_accept_key(const char *key, char *out)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)tmp, strlen(tmp), sha);
    EVP_EncodeBlock((unsigned char *)out, sha, SHA_DIGEST_LENGTH);
}

static void delay_fire(io_timer_t *t)
{
    bconn_t *c = CONTAINER_OF(t, bconn_t, delay);
    if (c->closed)
        return;
    db_put(&c->out, c->pending.p + c->pending.off, c->pending.len - c->pending.off);
    c->pending.len = c->pending.off = 0;
    c->st = c->keepalive ? B_HEAD : B_CLOSING;
    c->wr = true;
    conn_drive(c);
}

/* Genera la respuesta a la petición completa (cabecera + cuerpo en in). */
static void respond(bconn_t *c)
{
    atomic_fetch_add(&n_requests, 1);
    dbuf_t *o = opt.latency_ms ? &c->pending : &c->out;
    const char *body = c->in.p + c->body_start;
    size_t blen = c->in.off - c->body_start; /* cuerpo crudo ya consumido */
    int delay = opt.latency_ms;
    char buf[512];

    if (c->is_ws) {
        char acc[64];
        ws_accept_key(c->ws_key, acc);
        db_printf(&c->out,
                  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nX-Backend: %s\r\n\r\n",
                  acc, opt.name);
        c->st = B_WS;
        return;
    }
    if (strcmp(c->path, "/health") == 0) {
        simple(c, o, 200, "OK", "ok\n");
        delay = 0;
    } else if (strcmp(c->path, "/echo") == 0) {
        if (c->chunked_req) {
            resp_head(c, o, 200, "OK", "application/octet-stream", -1);
            if (!c->head_req)
                db_put(o, body, blen);
            else
                db_put(o, "0\r\n\r\n", 5);
        } else {
            resp_head(c, o, 200, "OK", "application/octet-stream", (long long)blen);
            if (!c->head_req)
                db_put(o, body, blen);
        }
    } else if (strcmp(c->path, "/headers") == 0) {
        resp_head(c, o, 200, "OK", "text/plain", (long long)c->head_copy_len);
        if (!c->head_req)
            db_put(o, c->head_copy, c->head_copy_len);
    } else if (strcmp(c->path, "/chunked") == 0) {
        resp_head(c, o, 200, "OK", "text/plain", -1);
        if (!c->head_req) {
            db_printf(o, "6\r\nhello \r\n");
            db_printf(o, "%zx;ext=1\r\nfrom %s\r\n", strlen(opt.name) + 5, opt.name);
            db_printf(o, "1\r\n\n\r\n0\r\nX-Trailer: yes\r\n\r\n");
        }
    } else if (strcmp(c->path, "/close") == 0) {
        c->keepalive = false;
        db_printf(o, "HTTP/1.1 200 OK\r\nX-Backend: %s\r\nContent-Type: text/plain\r\n"
                     "Connection: close\r\n\r\n", opt.name);
        if (!c->head_req)
            db_printf(o, "closed-delimited body from %s\n", opt.name);
    } else if (strcmp(c->path, "/nocontent") == 0) {
        resp_head(c, o, 204, "No Content", NULL, -2);
    } else if (strncmp(c->path, "/sleep/", 7) == 0) {
        delay = atoi(c->path + 7);
        snprintf(buf, sizeof(buf), "slept %d ms in %s\n", delay, opt.name);
        simple(c, o = &c->pending, 200, "OK", buf);
    } else if (strncmp(c->path, "/big/", 5) == 0) {
        long long n = atoll(c->path + 5);
        resp_head(c, o, 200, "OK", "application/octet-stream", n);
        if (!c->head_req) {
            char blk[8192];
            memset(blk, 'x', sizeof(blk));
            for (long long k = 0; k < n; k += (long long)sizeof(blk))
                db_put(o, blk, (size_t)MIN((long long)sizeof(blk), n - k));
        }
    } else if (strcmp(c->path, "/stats") == 0) {
        snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"connections\":%llu,\"requests\":%llu,\"active\":%d}\n",
                 opt.name, (unsigned long long)atomic_load(&n_conns),
                 (unsigned long long)atomic_load(&n_requests), atomic_load(&n_active));
        simple(c, o, 200, "OK", buf);
        delay = 0;
    } else {
        /* ~100 B, el tamaño de respuesta del benchmark (SPEC §11) */
        int n = snprintf(buf, sizeof(buf), "hello from %s path=%.60s\n", opt.name, c->path);
        if (n > 0 && n < 99) {
            memset(buf + n, '.', (size_t)(99 - n));
            buf[99] = '\n';
            buf[100] = '\0';
        }
        simple(c, o, 200, "OK", buf);
    }

    if (o == &c->pending && delay > 0) {
        c->st = B_WAIT;
        io_timer_after(c->t->loop, &c->delay, (uint64_t)delay);
    } else if (o == &c->pending) {
        db_put(&c->out, c->pending.p + c->pending.off, c->pending.len - c->pending.off);
        c->pending.len = c->pending.off = 0;
        c->st = c->keepalive ? B_HEAD : B_CLOSING;
    } else {
        c->st = c->keepalive ? B_HEAD : B_CLOSING;
    }
}

static bool process_ws(bconn_t *c)
{
    unsigned char *p = (unsigned char *)c->in.p + c->in.off;
    size_t n = c->in.len - c->in.off;
    if (n < 2)
        return false;
    int opcode = p[0] & 0x0f;
    bool masked = p[1] & 0x80;
    uint64_t len = p[1] & 0x7f;
    size_t hl = 2;
    if (len == 126) {
        if (n < 4)
            return false;
        len = ((uint64_t)p[2] << 8) | p[3];
        hl = 4;
    } else if (len == 127) {
        if (n < 10)
            return false;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | p[2 + i];
        hl = 10;
    }
    size_t mk = hl;
    if (masked)
        hl += 4;
    if (n < hl + len)
        return false;
    unsigned char *payload = p + hl;
    if (masked)
        for (uint64_t i = 0; i < len; i++)
            payload[i] ^= p[mk + (i & 3)];
    int reply_op = opcode == 9 ? 10 : opcode;
    unsigned char h[10];
    size_t rhl = 2;
    h[0] = (unsigned char)(0x80 | reply_op);
    if (len < 126) {
        h[1] = (unsigned char)len;
    } else if (len < 65536) {
        h[1] = 126;
        h[2] = (unsigned char)(len >> 8);
        h[3] = (unsigned char)len;
        rhl = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++)
            h[2 + i] = (unsigned char)(len >> (56 - 8 * i));
        rhl = 10;
    }
    if (opcode != 10) { /* no se contesta a pong */
        db_put(&c->out, h, rhl);
        db_put(&c->out, payload, (size_t)len);
    }
    c->in.off += hl + (size_t)len;
    if (opcode == 8)
        c->st = B_CLOSING;
    return true;
}

static bool process(bconn_t *c)
{
    if (c->st == B_WS)
        return process_ws(c);
    if (c->st == B_HEAD) {
        if (c->in.off == c->in.len)
            c->in.off = c->in.len = 0;
        size_t n = c->in.len - c->in.off;
        if (!n)
            return false;
        http_msg_t m;
        int r = http_parse_request(&m, c->in.p + c->in.off, n, 65536, &c->scan);
        if (r == HTTP_INCOMPLETE)
            return false;
        if (r != HTTP_COMPLETE) {
            c->keepalive = false;
            simple(c, &c->out, 400, "Bad Request", "bad request\n");
            c->st = B_CLOSING;
            return true;
        }
        c->nreq++;
        if (opt.drop_every > 0 && c->nreq > 1 && c->nreq % (unsigned)opt.drop_every == 0) {
            conn_close(c);
            return false;
        }
        c->keepalive = m.version_minor == 1 ? !m.conn_close : m.conn_keepalive;
        c->head_req = str_ieq(m.method, m.method_len, "HEAD");
        str_copy_n(c->path, sizeof(c->path), m.target, m.target_len);
        str_copy_n(c->method, sizeof(c->method), m.method, m.method_len);
        free(c->head_copy);
        c->head_copy = malloc(m.head_len);
        memcpy(c->head_copy, c->in.p + c->in.off, m.head_len);
        c->head_copy_len = m.head_len;
        c->is_ws = false;
        const http_header_t *wk = http_find(&m, "sec-websocket-key");
        if (strcmp(c->path, "/ws") == 0 && m.conn_upgrade && wk && wk->vlen < sizeof(c->ws_key)) {
            str_copy_n(c->ws_key, sizeof(c->ws_key), wk->value, wk->vlen);
            c->is_ws = true;
        }
        c->chunked_req = m.chunked;
        body_mode_t mode = m.chunked ? BODY_CHUNKED : m.content_length > 0 ? BODY_LENGTH : BODY_NONE;
        http_body_init(&c->body, mode, m.content_length > 0 ? (uint64_t)m.content_length : 0);
        c->in.off += m.head_len;
        c->body_start = c->in.off;
        c->scan = 0;
        c->st = B_BODY;
    }
    if (c->st == B_BODY) {
        size_t n = c->in.len - c->in.off;
        ssize_t k = http_body_feed(&c->body, c->in.p + c->in.off, n);
        if (k < 0) {
            c->keepalive = false;
            simple(c, &c->out, 400, "Bad Request", "bad chunked\n");
            c->st = B_CLOSING;
            return true;
        }
        c->in.off += (size_t)k;
        if (!c->body.done)
            return false;
        respond(c);
        /* Compactar: el cuerpo ya no hace falta. */
        if (c->in.off == c->in.len)
            c->in.off = c->in.len = 0;
        return true;
    }
    return false;
}

static void conn_drive(bconn_t *c)
{
    for (;;) {
        bool p = false;
        while (c->rd && !c->eof) {
            if (!db_reserve(&c->in, 16384)) {
                conn_close(c);
                return;
            }
            ssize_t n = recv(c->fd, c->in.p + c->in.len, c->in.cap - c->in.len, 0);
            if (n > 0) {
                c->in.len += (size_t)n;
                p = true;
            } else if (n == 0) {
                c->eof = true;
                p = true;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                c->rd = false;
            } else if (errno != EINTR) {
                conn_close(c);
                return;
            }
        }
        while ((c->st == B_HEAD || c->st == B_BODY || c->st == B_WS) && process(c))
            p = true;
        if (c->closed)
            return;
        while (c->wr && c->out.len > c->out.off) {
            ssize_t n = send(c->fd, c->out.p + c->out.off, c->out.len - c->out.off, MSG_NOSIGNAL);
            if (n > 0) {
                c->out.off += (size_t)n;
                p = true;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                c->wr = false;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                conn_close(c);
                return;
            }
        }
        if (c->out.off == c->out.len)
            c->out.off = c->out.len = 0;
        if (c->out.len == 0 && (c->st == B_CLOSING || (c->eof && c->st != B_WAIT))) {
            conn_close(c);
            return;
        }
        if (!p)
            return;
    }
}

static void conn_event(io_handler_t *h, uint32_t ev)
{
    bconn_t *c = (bconn_t *)h;
    if (c->closed)
        return;
    if (ev & (IO_READ | IO_HUP | IO_ERROR))
        c->rd = true;
    if (ev & (IO_WRITE | IO_ERROR))
        c->wr = true;
    conn_drive(c);
}

typedef struct {
    io_handler_t h;
    int fd;
    tctx_t *t;
} blisten_t;

static void accept_event(io_handler_t *h, uint32_t ev)
{
    (void)ev;
    blisten_t *l = (blisten_t *)h;
    for (;;) {
        int fd = accept(l->fd, NULL, NULL);
        if (fd < 0)
            break;
        set_nonblock(fd);
        set_nodelay(fd);
        bconn_t *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            continue;
        }
        c->h.on_event = conn_event;
        c->t = l->t;
        c->fd = fd;
        c->st = B_HEAD;
        c->rd = c->wr = true;
        io_timer_init(&c->delay, delay_fire);
        atomic_fetch_add(&n_conns, 1);
        atomic_fetch_add(&n_active, 1);
        io_loop_add(l->t->loop, fd, IO_READ | IO_WRITE, &c->h);
        conn_drive(c);
    }
}

static int open_listen(bool reuseport)
{
    char addr[128], err[256];
    snprintf(addr, sizeof(addr), "%s:%d", opt.bind_ip, opt.port);
    struct sockaddr_storage sa;
    socklen_t sl;
    if (addr_parse(addr, false, &sa, &sl, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (reuseport)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, sl) < 0 || listen(fd, 4096) < 0) {
        fprintf(stderr, "test_backend %s: bind/listen %s: %s\n", opt.name, addr, strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static int shared_fd = -1;

static void *thread_main(void *arg)
{
    (void)arg;
    tctx_t t = {.loop = io_loop_create(1024)};
    blisten_t l = {.h.on_event = accept_event, .t = &t};
#if defined(__linux__)
    l.fd = opt.threads > 1 ? open_listen(true) : shared_fd;
#else
    l.fd = shared_fd;
#endif
    if (l.fd < 0)
        exit(1);
    io_loop_add(t.loop, l.fd, IO_READ, &l.h);
    io_loop_run(t.loop);
    return NULL;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        {"port", required_argument, NULL, 'p'},
        {"name", required_argument, NULL, 'n'},
        {"latency-ms", required_argument, NULL, 'l'},
        {"load", required_argument, NULL, 'L'},
        {"load-auto", no_argument, NULL, 'A'},
        {"threads", required_argument, NULL, 't'},
        {"bind", required_argument, NULL, 'b'},
        {"drop-every", required_argument, NULL, 'D'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "p:n:l:L:At:b:D:h", longopts, NULL)) != -1) {
        switch (ch) {
        case 'p': opt.port = atoi(optarg); break;
        case 'n': str_copy(opt.name, optarg, sizeof(opt.name)); break;
        case 'l': opt.latency_ms = atoi(optarg); break;
        case 'L': opt.load = atof(optarg); opt.load_fixed = true; break;
        case 'A': opt.load_auto = true; break;
        case 't': opt.threads = MAX(1, atoi(optarg)); break;
        case 'b': str_copy(opt.bind_ip, optarg, sizeof(opt.bind_ip)); break;
        case 'D': opt.drop_every = atoi(optarg); break;
        default:
            fprintf(stderr,
                    "uso: %s --port N --name NOMBRE [--latency-ms N] [--load F|--load-auto]"
                    " [--threads N] [--bind IP] [--drop-every N]\n", argv[0]);
            return ch == 'h' ? 0 : 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    if (opt.threads == 1)
        shared_fd = open_listen(false);
    else {
        /* comprobar que el puerto está libre antes de lanzar hilos */
        int probe = open_listen(true);
        if (probe < 0)
            return 1;
        close(probe);
    }
#else
    shared_fd = open_listen(false);
#endif
    if (opt.threads == 1 && shared_fd < 0)
        return 1;
    fprintf(stderr, "test_backend %s escuchando en %s:%d (%d hilos)\n", opt.name,
            opt.bind_ip, opt.port, opt.threads);
    pthread_t th[64];
    int nt = MIN(opt.threads, 64);
    for (int i = 1; i < nt; i++)
        pthread_create(&th[i], NULL, thread_main, NULL);
    thread_main(NULL);
    return 0;
}
