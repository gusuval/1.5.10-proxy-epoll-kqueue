/*
 * Máquina de estados cliente <-> upstream.
 *
 * Cada sesión (conexión de cliente) tiene cuatro buffers de 16 KB:
 *   in  : bytes crudos del cliente (cabecera, cuerpo, peticiones pipelined)
 *   uo  : hacia el upstream (cabecera reescrita + cuerpo)
 *   ui  : bytes crudos del upstream
 *   out : hacia el cliente (cabecera reescrita + cuerpo)
 *
 * Todos los fds son edge-triggered: rd/wr indican si el último intento
 * terminó en EAGAIN. sess_drive() repite leer/procesar/escribir mientras
 * haya progreso, así nunca queda trabajo pendiente sin un evento que lo
 * despierte. Si una sesión agota su presupuesto se vuelve a planificar al
 * final de la iteración para no acaparar el loop.
 */
#include "worker.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "http_parser.h"
#include "log.h"
#include "util.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HEAD_MAX BUF_SIZE
#define DRIVE_BUDGET 32
#define MAX_ATTEMPTS 3
#define CLOSING_MS 5000
#define LINGER_MS 2000

typedef enum {
    S_HANDSHAKE,
    S_HEAD,
    S_PROXY,
    S_TUNNEL,
    S_CLOSING, /* vaciando out antes de cerrar */
    S_LINGER,  /* write cerrado; descartando lo que llegue hasta EOF */
} sstate_t;

typedef struct upstream {
    io_handler_t h;
    int fd;
    server_t *srv;
    backend_t *be;
    struct session *sess; /* NULL si está ocioso en el pool */
    struct upstream *pnext, *pprev;
    io_timer_t timer;
    bool connecting, rd, wr, eof, closed, reused, in_pool;
} upstream_t;

typedef struct session {
    io_handler_t h;
    int fd;
    SSL *ssl;
    sstate_t st;
    bool rd, wr, rd_needs_wr, wr_needs_rd, eof, closed, redrive;

    bool cli_keepalive, client_v10, head_req, want_upgrade, retry_ok;
    bool req_done, resp_head_done, resp_done, up_reusable, resp_started;
    bool head_timer;
    int status;

    char fe_key[CFG_ADDR_MAX];
    char ip[48];
    char sni[CFG_HOST_MAX];
    char cert_pat[CFG_HOST_MAX];

    buf_t in, out, uo, ui;
    size_t scan_req, scan_resp;
    http_body_t reqb, respb;

    upstream_t *up;
    runtime_t *rt;
    backend_t *be;
    uint64_t tried;
    int attempts;
    bool force_new;

    uint64_t t_accept, t_state, t_head, t_req, t_conn, t_progress;
    char log_method[16];
    char log_host[64];
    char log_path[160];
    char log_server[CFG_ADDR_MAX];

    io_timer_t timer;
    struct session *prev, *next;
} session_t;

enum { UF_CONNECT, UF_CONNECT_TIMEOUT, UF_IO };

static void sess_drive(session_t *s);
static void sess_close(session_t *s);
static void send_error(session_t *s, int code);
static bool connect_upstream(session_t *s);

static inline uint64_t now_ms(void) { return io_loop_now(W.loop); }
static inline const cfg_timeouts_t *TO(void) { return &W.rt->cfg->timeouts; }

/* ------------------------------------------------------------------ */
/* arena de sesiones                                                    */
/* ------------------------------------------------------------------ */

static session_t *free_sessions;

static session_t *sess_alloc(void)
{
    if (!free_sessions) {
        enum { CHUNK = 256 };
        session_t *c = calloc(CHUNK, sizeof(session_t));
        if (!c)
            return NULL;
        for (int i = 0; i < CHUNK; i++) {
            c[i].next = free_sessions;
            free_sessions = &c[i];
        }
    }
    session_t *s = free_sessions;
    free_sessions = s->next;
    memset(s, 0, sizeof(*s));
    return s;
}

static void sess_free_deferred(void *p)
{
    session_t *s = p;
    s->next = free_sessions;
    free_sessions = s;
}

/* ------------------------------------------------------------------ */
/* upstreams y pool keep-alive                                          */
/* ------------------------------------------------------------------ */

static void up_free_deferred(void *p) { free(p); }

static void pool_unlink(upstream_t *u)
{
    server_t *srv = u->srv;
    if (u->pprev)
        u->pprev->pnext = u->pnext;
    else
        srv->idle_head = u->pnext;
    if (u->pnext)
        u->pnext->pprev = u->pprev;
    u->pnext = u->pprev = NULL;
    u->in_pool = false;
    srv->idle_count--;
}

static void up_close(upstream_t *u)
{
    if (u->closed)
        return;
    u->closed = true;
    io_timer_cancel(W.loop, &u->timer);
    if (u->in_pool)
        pool_unlink(u);
    io_loop_del(W.loop, u->fd);
    close(u->fd);
    io_loop_defer(W.loop, up_free_deferred, u);
}

static void up_idle_timeout(io_timer_t *t)
{
    up_close(CONTAINER_OF(t, upstream_t, timer));
}

static void pool_put(upstream_t *u)
{
    server_t *srv = u->srv;
    if (srv->idle_count >= u->be->max_idle) {
        up_close(u);
        return;
    }
    u->sess = NULL;
    u->reused = true;
    u->pprev = NULL;
    u->pnext = srv->idle_head;
    if (u->pnext)
        ((upstream_t *)u->pnext)->pprev = u;
    srv->idle_head = u;
    srv->idle_count++;
    u->in_pool = true;
    io_timer_after(W.loop, &u->timer, (uint64_t)TO()->upstream_idle);
}

static upstream_t *pool_get(server_t *srv)
{
    upstream_t *u = srv->idle_head;
    if (!u)
        return NULL;
    pool_unlink(u);
    io_timer_cancel(W.loop, &u->timer);
    return u;
}

void upstream_pool_drain(server_t *srv)
{
    while (srv->idle_head)
        up_close(srv->idle_head);
}

static void up_event(io_handler_t *h, uint32_t ev)
{
    upstream_t *u = (upstream_t *)h;
    if (u->closed)
        return;
    if (ev & (IO_READ | IO_HUP | IO_ERROR))
        u->rd = true;
    if (ev & (IO_WRITE | IO_ERROR))
        u->wr = true;
    if (!u->sess) {
        /* Ocioso en el pool: cualquier dato o cierre invalida la conexión.
         * El evento puede ser tardío (datos ya leídos), así que se mira. */
        if (ev & (IO_HUP | IO_ERROR)) {
            up_close(u);
        } else if (ev & IO_READ) {
            char c;
            ssize_t n = recv(u->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                up_close(u);
            else
                u->rd = false;
        }
        return;
    }
    sess_drive(u->sess);
}

static void up_attach(session_t *s, upstream_t *u)
{
    s->up = u;
    u->sess = s;
    u->srv->active++;
    u->srv->requests++;
    str_copy(s->log_server, u->srv->addr_str, sizeof(s->log_server));
    s->up_reusable = true;
    s->resp_started = false;
    s->ui.r = s->ui.w = 0;
    s->scan_resp = 0;
}

/* Suelta el upstream actual sin devolverlo al pool. */
static void up_detach_close(session_t *s)
{
    upstream_t *u = s->up;
    if (!u)
        return;
    u->srv->active--;
    s->up = NULL;
    u->sess = NULL;
    up_close(u);
}

static bool connect_upstream(session_t *s)
{
    uint64_t now = now_ms();
    for (;;) {
        server_t *srv = backend_pick(s->be, s->tried, now);
        if (!srv) {
            if (s->attempts == 0)
                LOGW("backend '%s' sin servidores disponibles", s->be->name);
            send_error(s, s->attempts ? 502 : 503);
            return false;
        }
        if (srv->idx < 64)
            s->tried |= 1ULL << srv->idx;
        s->attempts++;
        s->t_conn = now;

        upstream_t *u = s->force_new ? NULL : pool_get(srv);
        if (u) {
            stat_inc(&W.st->upstream_reuses);
            up_attach(s, u);
            return true;
        }
        int fd = socket(srv->sa.ss_family, SOCK_STREAM, 0);
        if (fd < 0) {
            LOGE("socket(): %s", strerror(errno));
            send_error(s, 502);
            return false;
        }
        set_nonblock(fd);
        set_cloexec(fd);
        set_nodelay(fd);
        int r = connect(fd, (struct sockaddr *)&srv->sa, srv->salen);
        if (r < 0 && errno != EINPROGRESS) {
            int e = errno;
            close(fd);
            LOGW("connect %s (%s): %s", srv->addr_str, s->be->name, strerror(e));
            if (server_passive_fail(s->be, srv, now))
                LOGW("backend '%s' servidor %s -> DOWN (fallos pasivos)", s->be->name,
                     srv->addr_str);
            if (s->attempts >= MAX_ATTEMPTS) {
                send_error(s, 502);
                return false;
            }
            continue;
        }
        u = calloc(1, sizeof(*u));
        if (!u) {
            close(fd);
            send_error(s, 502);
            return false;
        }
        u->h.on_event = up_event;
        u->fd = fd;
        u->srv = srv;
        u->be = s->be;
        u->connecting = r < 0;
        u->wr = r == 0;
        io_timer_init(&u->timer, up_idle_timeout);
        if (io_loop_add(W.loop, fd, IO_READ | IO_WRITE, &u->h) < 0) {
            close(fd);
            free(u);
            send_error(s, 502);
            return false;
        }
        stat_inc(&W.st->upstream_connects);
        up_attach(s, u);
        return true;
    }
}

static void upstream_failed(session_t *s, int why)
{
    upstream_t *u = s->up;
    if (!u)
        return;
    server_t *srv = u->srv;
    backend_t *be = s->be;
    bool reused = u->reused;
    bool got_nothing = !s->resp_started;
    uint64_t now = now_ms();
    up_detach_close(s);

    if (why == UF_CONNECT || why == UF_CONNECT_TIMEOUT) {
        LOGW("upstream %s (%s): %s", srv->addr_str, be->name,
             why == UF_CONNECT ? "conexión rechazada" : "timeout de conexión");
        if (server_passive_fail(be, srv, now))
            LOGW("backend '%s' servidor %s -> DOWN (fallos pasivos)", be->name, srv->addr_str);
        /* Nada se ha enviado todavía: se puede probar otro servidor. */
        if (s->attempts < MAX_ATTEMPTS) {
            s->uo.r = 0;
            connect_upstream(s);
            return;
        }
        send_error(s, why == UF_CONNECT ? 502 : 504);
        return;
    }
    /* Conexión reutilizada que el backend cerró mientras estaba ociosa:
     * reintento único en conexión nueva si es idempotente y sin cuerpo. */
    if (reused && got_nothing && s->retry_ok) {
        stat_inc(&W.st->upstream_retries);
        s->retry_ok = false;
        s->force_new = true;
        s->uo.r = 0;
        s->tried = 0;
        s->attempts = 0;
        connect_upstream(s);
        return;
    }
    if (!reused && got_nothing && server_passive_fail(be, srv, now))
        LOGW("backend '%s' servidor %s -> DOWN (fallos pasivos)", be->name, srv->addr_str);
    if (s->resp_head_done) {
        sess_close(s); /* respuesta a medias: solo queda cortar */
        return;
    }
    send_error(s, 502);
}

/* E/S con el upstream: connect, escritura de uo y lectura a ui. */
static bool up_io(session_t *s)
{
    upstream_t *u = s->up;
    if (!u)
        return false;
    bool p = false;
    if (u->connecting) {
        if (!u->wr && !u->rd)
            return false;
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0)
            err = errno;
        if (err) {
            upstream_failed(s, UF_CONNECT);
            return true;
        }
        u->connecting = false;
        u->wr = true;
        p = true;
    }
    while (u->wr && buf_len(&s->uo)) {
        ssize_t n = send(u->fd, buf_rptr(&s->uo), buf_len(&s->uo), MSG_NOSIGNAL);
        if (n > 0) {
            /* Con reintento posible se conserva la cabecera enviada. */
            if (s->retry_ok)
                s->uo.r += (uint32_t)n;
            else
                buf_consume(&s->uo, (size_t)n);
            p = true;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            u->wr = false;
            break;
        }
        upstream_failed(s, UF_IO);
        return true;
    }
    while (u->rd && !u->eof) {
        if (!buf_space(&s->ui)) {
            buf_compact(&s->ui);
            if (!buf_space(&s->ui))
                break;
        }
        ssize_t n = recv(u->fd, buf_wptr(&s->ui), buf_space(&s->ui), 0);
        if (n > 0) {
            s->ui.w += (uint32_t)n;
            s->resp_started = true;
            p = true;
            continue;
        }
        if (n == 0) {
            u->eof = true;
            p = true;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            u->rd = false;
            break;
        }
        /* ECONNRESET y similares */
        if (!s->resp_started) {
            upstream_failed(s, UF_IO);
            return true;
        }
        u->eof = true;
        s->up_reusable = false;
        p = true;
        break;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* cliente                                                              */
/* ------------------------------------------------------------------ */

static void account(session_t *s, int status)
{
    stat_inc(&W.st->requests);
    int c = status / 100 - 1;
    if (c >= 0 && c < ST_NCLASS)
        stat_inc(&W.st->resp[c]);
    if (W.rt->cfg->access_log) {
        uint64_t ms = s->t_req ? now_ms() - s->t_req : 0;
        log_msg(LOG_ACCESS, "%s %s \"%s %s\" %d %s %llums", s->ip,
                s->log_host[0] ? s->log_host : "-",
                s->log_method[0] ? s->log_method : "-",
                s->log_path[0] ? s->log_path : "-", status,
                s->log_server[0] ? s->log_server : "-", (unsigned long long)ms);
    }
}

static void release_up_bufs(session_t *s)
{
    buf_release(W.bp, &s->uo);
    buf_release(W.bp, &s->ui);
}

static void enter_head(session_t *s)
{
    s->st = S_HEAD;
    s->t_state = now_ms();
    s->scan_req = 0;
    s->head_timer = false;
    s->t_req = 0;
    s->log_method[0] = s->log_host[0] = s->log_path[0] = s->log_server[0] = '\0';
    release_up_bufs(s);
    if (s->in.data && !buf_len(&s->in))
        buf_release(W.bp, &s->in);
    if (s->out.data && !buf_len(&s->out))
        buf_release(W.bp, &s->out);
}

static void enter_closing(session_t *s)
{
    s->st = S_CLOSING;
    s->t_state = now_ms();
    s->cli_keepalive = false;
    if (s->in.data)
        s->in.r = s->in.w = 0;
    release_up_bufs(s);
    if (s->rt) {
        rt_unref(s->rt);
        s->rt = NULL;
    }
    s->be = NULL;
}

static void send_error(session_t *s, int code)
{
    up_detach_close(s);
    if (s->resp_head_done) {
        sess_close(s);
        return;
    }
    s->status = code;
    const char *reason = http_reason(code);
    if (!buf_ensure(W.bp, &s->out)) {
        sess_close(s);
        return;
    }
    buf_compact(&s->out);
    int n = snprintf(buf_wptr(&s->out), buf_space(&s->out),
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s\n",
                     code, reason, strlen(reason) + 1, reason);
    if (n < 0 || (size_t)n >= buf_space(&s->out)) {
        sess_close(s);
        return;
    }
    s->out.w += (uint32_t)n;
    account(s, code);
    enter_closing(s);
}

static void sess_close(session_t *s)
{
    if (s->closed)
        return;
    s->closed = true;
    io_timer_cancel(W.loop, &s->timer);
    up_detach_close(s);
    if (s->rt) {
        rt_unref(s->rt);
        s->rt = NULL;
    }
    if (s->ssl) {
        SSL_free(s->ssl);
        s->ssl = NULL;
    }
    io_loop_del(W.loop, s->fd);
    close(s->fd);
    buf_release(W.bp, &s->in);
    buf_release(W.bp, &s->out);
    release_up_bufs(s);
    if (s->prev)
        s->prev->next = s->next;
    else
        W.sessions = s->next;
    if (s->next)
        s->next->prev = s->prev;
    W.nsessions--;
    atomic_fetch_sub_explicit(&W.st->active_conns, 1, memory_order_relaxed);
    io_loop_defer(W.loop, sess_free_deferred, s);
}

static bool cli_read(session_t *s)
{
    if (!s->rd || s->eof || s->st == S_CLOSING)
        return false;
    if (!buf_ensure(W.bp, &s->in)) {
        sess_close(s);
        return true;
    }
    bool p = false;
    for (;;) {
        if (!buf_space(&s->in)) {
            buf_compact(&s->in);
            if (!buf_space(&s->in))
                break;
        }
        size_t space = buf_space(&s->in);
        ssize_t n;
        if (s->ssl) {
            ERR_clear_error();
            int r = SSL_read(s->ssl, buf_wptr(&s->in), (int)space);
            if (r > 0) {
                n = r;
            } else {
                int e = SSL_get_error(s->ssl, r);
                if (e == SSL_ERROR_WANT_READ) {
                    s->rd = false;
                } else if (e == SSL_ERROR_WANT_WRITE) {
                    s->rd = false;
                    s->rd_needs_wr = true;
                } else if (e == SSL_ERROR_ZERO_RETURN) {
                    s->eof = true;
                    p = true;
                } else {
                    sess_close(s);
                    return true;
                }
                break;
            }
        } else {
            n = recv(s->fd, buf_wptr(&s->in), space, 0);
            if (n == 0) {
                s->eof = true;
                p = true;
                break;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    s->rd = false;
                    break;
                }
                sess_close(s);
                return true;
            }
        }
        s->in.w += (uint32_t)n;
        p = true;
    }
    return p;
}

static bool cli_write(session_t *s)
{
    bool p = false;
    while (s->wr && buf_len(&s->out)) {
        size_t len = buf_len(&s->out);
        if (s->ssl) {
            ERR_clear_error();
            int r = SSL_write(s->ssl, buf_rptr(&s->out), (int)len);
            if (r > 0) {
                buf_consume(&s->out, (size_t)r);
                p = true;
                continue;
            }
            int e = SSL_get_error(s->ssl, r);
            if (e == SSL_ERROR_WANT_WRITE) {
                s->wr = false;
            } else if (e == SSL_ERROR_WANT_READ) {
                s->wr = false;
                s->wr_needs_rd = true;
            } else {
                sess_close(s);
                return true;
            }
            break;
        }
        ssize_t n = send(s->fd, buf_rptr(&s->out), len, MSG_NOSIGNAL);
        if (n > 0) {
            buf_consume(&s->out, (size_t)n);
            p = true;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            s->wr = false;
            break;
        }
        sess_close(s);
        return true;
    }
    if (s->out.data && !buf_len(&s->out) && s->st == S_HEAD && !buf_len(&s->in))
        buf_release(W.bp, &s->out);
    return p;
}

static bool do_handshake(session_t *s)
{
    ERR_clear_error();
    int r = SSL_do_handshake(s->ssl);
    if (r == 1) {
        const char *sn = SSL_get_servername(s->ssl, TLSEXT_NAMETYPE_host_name);
        if (sn)
            str_copy(s->sni, sn, sizeof(s->sni));
        const char *pat = tls_served_pattern(s->ssl);
        if (pat)
            str_copy(s->cert_pat, pat, sizeof(s->cert_pat));
        if (s->rt) {
            rt_unref(s->rt);
            s->rt = NULL;
        }
        stat_inc(&W.st->tls_handshakes);
        enter_head(s);
        s->rd = s->wr = true;
        return true;
    }
    int e = SSL_get_error(s->ssl, r);
    if (e == SSL_ERROR_WANT_READ) {
        s->rd = false;
        return false;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        s->wr = false;
        return false;
    }
    char eb[160] = "";
    unsigned long ec = ERR_peek_last_error();
    if (ec)
        ERR_error_string_n(ec, eb, sizeof(eb));
    LOGD("handshake TLS fallido desde %s: %s", s->ip, eb[0] ? eb : "EOF");
    sess_close(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* reescritura de cabeceras                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t cap, len;
    bool overflow;
} wb_t;

static void wb_put(wb_t *w, const char *s, size_t n)
{
    if (w->overflow || w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->p + w->len, s, n);
    w->len += n;
}

static void wb_str(wb_t *w, const char *s) { wb_put(w, s, strlen(s)); }

static bool is_idempotent(const char *m, size_t n)
{
    return str_ieq(m, n, "GET") || str_ieq(m, n, "HEAD") || str_ieq(m, n, "PUT") ||
           str_ieq(m, n, "DELETE") || str_ieq(m, n, "OPTIONS") || str_ieq(m, n, "TRACE");
}

static int build_request_head(session_t *s, const http_msg_t *m, const char *fallback_host)
{
    wb_t w = {.p = buf_wptr(&s->uo), .cap = buf_space(&s->uo)};
    wb_put(&w, m->method, m->method_len);
    wb_put(&w, " ", 1);
    wb_put(&w, m->target, m->target_len);
    wb_str(&w, " HTTP/1.1\r\n");

    int last_xff = -1;
    for (int i = 0; i < m->nheaders; i++)
        if (str_ieq(m->headers[i].name, m->headers[i].nlen, "x-forwarded-for"))
            last_xff = i;

    for (int i = 0; i < m->nheaders; i++) {
        const http_header_t *h = &m->headers[i];
        if (http_is_hop_by_hop(m, h->name, h->nlen) ||
            str_ieq(h->name, h->nlen, "x-forwarded-proto") ||
            str_ieq(h->name, h->nlen, "x-real-ip"))
            continue;
        wb_put(&w, h->name, h->nlen);
        wb_put(&w, ": ", 2);
        wb_put(&w, h->value, h->vlen);
        if (i == last_xff) {
            wb_put(&w, ", ", 2);
            wb_str(&w, s->ip);
        }
        wb_put(&w, "\r\n", 2);
    }
    if (!m->has_host) {
        wb_str(&w, "Host: ");
        wb_str(&w, fallback_host);
        wb_put(&w, "\r\n", 2);
    }
    if (last_xff < 0) {
        wb_str(&w, "X-Forwarded-For: ");
        wb_str(&w, s->ip);
        wb_put(&w, "\r\n", 2);
    }
    wb_str(&w, s->ssl ? "X-Forwarded-Proto: https\r\n" : "X-Forwarded-Proto: http\r\n");
    wb_str(&w, "X-Real-IP: ");
    wb_str(&w, s->ip);
    wb_put(&w, "\r\n", 2);
    if (s->want_upgrade) {
        wb_str(&w, "Connection: Upgrade\r\nUpgrade: ");
        wb_put(&w, m->upgrade, m->upgrade_len);
        wb_put(&w, "\r\n", 2);
    }
    wb_put(&w, "\r\n", 2);
    if (w.overflow)
        return -1;
    s->uo.w += (uint32_t)w.len;
    return 0;
}

/* Escribe la cabecera de respuesta reescrita en out. 0 ok, 1 sin sitio, -1 no cabe nunca. */
static int build_response_head(session_t *s, const http_msg_t *m, bool interim)
{
    if (!buf_ensure(W.bp, &s->out))
        return -1;
    buf_compact(&s->out);
    wb_t w = {.p = buf_wptr(&s->out), .cap = buf_space(&s->out)};
    char line[64];
    snprintf(line, sizeof(line), "HTTP/1.1 %03d ", m->status);
    wb_str(&w, line);
    wb_put(&w, m->reason, m->reason_len);
    wb_put(&w, "\r\n", 2);
    bool upgrade = m->status == 101;
    for (int i = 0; i < m->nheaders; i++) {
        const http_header_t *h = &m->headers[i];
        if (str_ieq(h->name, h->nlen, "x-backend-load"))
            continue;
        if (upgrade && (str_ieq(h->name, h->nlen, "connection") ||
                        str_ieq(h->name, h->nlen, "upgrade"))) {
            /* se conservan en el 101 */
        } else if (http_is_hop_by_hop(m, h->name, h->nlen)) {
            continue;
        }
        wb_put(&w, h->name, h->nlen);
        wb_put(&w, ": ", 2);
        wb_put(&w, h->value, h->vlen);
        wb_put(&w, "\r\n", 2);
    }
    if (!interim && !upgrade) {
        if (!s->cli_keepalive)
            wb_str(&w, "Connection: close\r\n");
        else if (s->client_v10)
            wb_str(&w, "Connection: keep-alive\r\n");
    }
    wb_put(&w, "\r\n", 2);
    if (w.overflow)
        return buf_len(&s->out) ? 1 : -1;
    s->out.w += (uint32_t)w.len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* procesamiento por estado                                             */
/* ------------------------------------------------------------------ */

static bool dispatch(session_t *s, const http_msg_t *m)
{
    uint64_t now = now_ms();
    s->t_req = now;
    s->status = 0;
    str_copy_n(s->log_method, sizeof(s->log_method), m->method, m->method_len);
    str_copy_n(s->log_path, sizeof(s->log_path), m->target, m->target_len);
    char host[CFG_HOST_MAX];
    str_copy_n(host, sizeof(host), m->host ? m->host : "", m->host_len);
    str_lower(host);
    size_t hl = strlen(host);
    str_copy(s->log_host, host, sizeof(s->log_host));

    s->client_v10 = m->version_minor == 0;
    s->cli_keepalive = m->version_minor == 1 ? !m->conn_close : m->conn_keepalive;
    if (W.stopping)
        s->cli_keepalive = false;
    s->head_req = str_ieq(m->method, m->method_len, "HEAD");
    s->want_upgrade = m->conn_upgrade && m->upgrade_len > 0;

    s->rt = W.rt;
    rt_ref(s->rt);
    frontend_t *fe = runtime_frontend(s->rt, s->fe_key);
    if (!fe || fe->tls != (s->ssl != NULL)) {
        send_error(s, 503); /* frontend eliminado en una recarga */
        return true;
    }
    if (s->ssl && s->sni[0] && hl && strcasecmp(host, s->sni) != 0 &&
        !(s->cert_pat[0] && tls_pattern_match(s->cert_pat, host, hl))) {
        send_error(s, 421);
        return true;
    }
    int bi = router_lookup(fe->router, host, hl);
    if (bi < 0) {
        send_error(s, 404);
        return true;
    }
    s->be = &s->rt->bes[bi];

    body_mode_t mode = BODY_NONE;
    uint64_t cl = 0;
    if (m->chunked) {
        mode = BODY_CHUNKED;
    } else if (m->content_length > 0) {
        mode = BODY_LENGTH;
        cl = (uint64_t)m->content_length;
    }
    http_body_init(&s->reqb, mode, cl);
    s->req_done = s->reqb.done;
    s->retry_ok = mode == BODY_NONE && is_idempotent(m->method, m->method_len);

    if (!buf_ensure(W.bp, &s->uo) || !buf_ensure(W.bp, &s->ui)) {
        sess_close(s);
        return true;
    }
    if (build_request_head(s, m, fe->key) < 0) {
        send_error(s, 431);
        return true;
    }
    buf_consume(&s->in, m->head_len);
    s->scan_req = 0;
    s->st = S_PROXY;
    s->t_state = now;
    s->resp_head_done = s->resp_done = false;
    s->tried = 0;
    s->attempts = 0;
    s->force_new = false;
    connect_upstream(s);
    return true;
}

static bool proc_head(session_t *s)
{
    size_t len = s->in.data ? buf_len(&s->in) : 0;
    if (len == 0) {
        if (s->eof || W.stopping) {
            if (s->out.data && buf_len(&s->out))
                enter_closing(s);
            else
                sess_close(s);
            return true;
        }
        return false;
    }
    if (!s->head_timer) {
        s->head_timer = true;
        s->t_head = now_ms();
    }
    http_msg_t m;
    int r = http_parse_request(&m, buf_rptr(&s->in), len, HEAD_MAX, &s->scan_req);
    if (r == HTTP_INCOMPLETE) {
        if (len >= BUF_SIZE) {
            r = HTTP_TOO_LARGE;
        } else {
            if (s->eof)
                sess_close(s);
            return false;
        }
    }
    if (r == HTTP_TOO_LARGE) {
        send_error(s, 431);
        return true;
    }
    if (r == HTTP_ERROR) {
        send_error(s, 400);
        return true;
    }
    return dispatch(s, &m);
}

static void finish_request(session_t *s)
{
    upstream_t *u = s->up;
    bool reusable = u && s->up_reusable && !u->eof && !buf_len(&s->ui) && s->req_done &&
                    s->respb.mode != BODY_EOF && !W.stopping;
    if (u) {
        server_passive_ok(u->srv);
        u->srv->active--;
        s->up = NULL;
        if (reusable)
            pool_put(u);
        else
            up_close(u);
    }
    account(s, s->status);
    if (s->rt) {
        rt_unref(s->rt);
        s->rt = NULL;
    }
    s->be = NULL;
    if (!s->cli_keepalive || !s->req_done) {
        enter_closing(s);
        return;
    }
    enter_head(s);
}

static bool proc_response_head(session_t *s)
{
    upstream_t *u = s->up;
    size_t len = buf_len(&s->ui);
    if (!len) {
        if (u->eof) {
            upstream_failed(s, UF_IO);
            return true;
        }
        return false;
    }
    http_msg_t m;
    int r = http_parse_response(&m, buf_rptr(&s->ui), len, HEAD_MAX, &s->scan_resp);
    if (r == HTTP_INCOMPLETE) {
        if (len >= BUF_SIZE)
            r = HTTP_TOO_LARGE;
        else if (u->eof)
            r = HTTP_ERROR;
        else
            return false;
    }
    if (r != HTTP_COMPLETE) {
        LOGW("respuesta inválida de %s (%s)", u->srv->addr_str, s->be->name);
        s->resp_started = true; /* no reintentar */
        up_detach_close(s);
        send_error(s, 502);
        return true;
    }
    s->retry_ok = false;
    if (s->uo.r == s->uo.w)
        s->uo.r = s->uo.w = 0;

    if (m.status >= 100 && m.status < 200 && m.status != 101) {
        /* respuesta intermedia (100 Continue...): se reenvía salvo a HTTP/1.0 */
        if (!s->client_v10) {
            int b = build_response_head(s, &m, true);
            if (b > 0)
                return false;
            if (b < 0) {
                send_error(s, 502);
                return true;
            }
        }
        buf_consume(&s->ui, m.head_len);
        s->scan_resp = 0;
        return true;
    }
    if (m.status == 101) {
        if (!s->want_upgrade) {
            up_detach_close(s);
            send_error(s, 502);
            return true;
        }
        int b = build_response_head(s, &m, false);
        if (b > 0)
            return false;
        if (b < 0) {
            send_error(s, 502);
            return true;
        }
        buf_consume(&s->ui, m.head_len);
        s->status = 101;
        s->resp_head_done = true;
        s->st = S_TUNNEL;
        s->t_state = now_ms();
        return true;
    }

    if (m.backend_load) {
        double v;
        if (parse_load(m.backend_load, m.backend_load_len, &v))
            server_report_load(u->srv, v, now_ms());
    }
    body_mode_t mode;
    uint64_t cl = 0;
    if (s->head_req || m.status == 204 || m.status == 304) {
        mode = BODY_NONE;
    } else if (m.chunked) {
        mode = BODY_CHUNKED;
    } else if (m.content_length >= 0) {
        mode = m.content_length ? BODY_LENGTH : BODY_NONE;
        cl = (uint64_t)m.content_length;
    } else {
        mode = BODY_EOF;
    }
    if (mode == BODY_EOF) {
        s->cli_keepalive = false;
        s->up_reusable = false;
    }
    if (m.conn_close || (m.version_minor == 0 && !m.conn_keepalive))
        s->up_reusable = false;
    if (!s->req_done)
        s->cli_keepalive = false; /* respuesta antes de terminar el cuerpo */

    int b = build_response_head(s, &m, false);
    if (b > 0)
        return false;
    if (b < 0) {
        send_error(s, 502);
        return true;
    }
    buf_consume(&s->ui, m.head_len);
    s->status = m.status;
    s->resp_head_done = true;
    http_body_init(&s->respb, mode, cl);
    s->resp_done = s->respb.done;
    return true;
}

static bool proc_proxy(session_t *s)
{
    bool p = false;

    /* cuerpo de la petición: in -> uo */
    if (!s->req_done && s->in.data && buf_len(&s->in)) {
        if (!buf_space(&s->uo))
            buf_compact(&s->uo);
        size_t n = MIN(buf_len(&s->in), buf_space(&s->uo));
        if (n) {
            ssize_t k = http_body_feed(&s->reqb, buf_rptr(&s->in), n);
            if (k < 0) {
                send_error(s, 400);
                return true;
            }
            memcpy(buf_wptr(&s->uo), buf_rptr(&s->in), (size_t)k);
            s->uo.w += (uint32_t)k;
            buf_consume(&s->in, (size_t)k);
            if (s->reqb.done)
                s->req_done = true;
            if (k > 0 || s->req_done)
                p = true;
        }
    }
    if (!s->req_done && s->eof && (!s->in.data || !buf_len(&s->in))) {
        sess_close(s); /* el cliente cortó a mitad del cuerpo */
        return true;
    }
    if (!s->up)
        return p;

    if (!s->resp_head_done) {
        if (!proc_response_head(s))
            return p;
        p = true;
        if (s->closed || s->st != S_PROXY || !s->resp_head_done)
            return p;
    }

    /* cuerpo de la respuesta: ui -> out */
    upstream_t *u = s->up;
    if (!s->resp_done && buf_len(&s->ui)) {
        if (!buf_space(&s->out))
            buf_compact(&s->out);
        size_t n = MIN(buf_len(&s->ui), buf_space(&s->out));
        if (n) {
            ssize_t k = http_body_feed(&s->respb, buf_rptr(&s->ui), n);
            if (k < 0) {
                LOGW("chunked inválido desde %s", u->srv->addr_str);
                s->up_reusable = false;
                sess_close(s);
                return true;
            }
            memcpy(buf_wptr(&s->out), buf_rptr(&s->ui), (size_t)k);
            s->out.w += (uint32_t)k;
            buf_consume(&s->ui, (size_t)k);
            if (s->respb.done)
                s->resp_done = true;
            if (k > 0 || s->resp_done)
                p = true;
        }
    }
    if (!s->resp_done && u->eof && !buf_len(&s->ui)) {
        if (s->respb.mode == BODY_EOF) {
            s->resp_done = true;
        } else {
            /* respuesta truncada: se entrega lo que hay y se cierra */
            up_detach_close(s);
            enter_closing(s);
            return true;
        }
    }
    if (s->resp_done && s->req_done) {
        finish_request(s);
        return true;
    }
    if (s->resp_done && !s->req_done) {
        /* El backend respondió sin esperar al cuerpo: no se puede reutilizar
         * ninguna de las dos conexiones. */
        s->up_reusable = false;
        finish_request(s);
        return true;
    }
    return p;
}

static bool proc_tunnel(session_t *s)
{
    bool p = false;
    upstream_t *u = s->up;
    if (s->in.data && buf_len(&s->in) && buf_move(&s->uo, &s->in))
        p = true;
    if (buf_len(&s->ui)) {
        if (!buf_ensure(W.bp, &s->out)) {
            sess_close(s);
            return true;
        }
        if (buf_move(&s->out, &s->ui))
            p = true;
    }
    bool cli_done = s->eof && (!s->in.data || !buf_len(&s->in)) && !buf_len(&s->uo);
    bool up_done = !u || (u->eof && !buf_len(&s->ui));
    if (cli_done || up_done) {
        account(s, 101);
        up_detach_close(s);
        enter_closing(s);
        return true;
    }
    return p;
}

static bool process(session_t *s)
{
    switch (s->st) {
    case S_HEAD:
        return proc_head(s);
    case S_PROXY:
        return proc_proxy(s);
    case S_TUNNEL:
        return proc_tunnel(s);
    case S_LINGER:
        if (s->in.data)
            s->in.r = s->in.w = 0;
        if (s->eof) {
            sess_close(s);
            return true;
        }
        return false;
    case S_HANDSHAKE:
    case S_CLOSING:
        return false;
    }
    return false;
}

static void start_linger(session_t *s)
{
    if (s->ssl)
        SSL_shutdown(s->ssl); /* close_notify; no se espera respuesta */
    shutdown(s->fd, SHUT_WR);
    s->st = S_LINGER;
    s->t_state = now_ms();
    if (s->eof)
        sess_close(s);
}

static void update_timer(session_t *s)
{
    const cfg_timeouts_t *t = TO();
    uint64_t dl;
    switch (s->st) {
    case S_HANDSHAKE:
        dl = s->t_accept + (uint64_t)t->client_header;
        break;
    case S_HEAD:
        if (s->head_timer)
            dl = s->t_head + (uint64_t)t->client_header;
        else if (s->out.data && buf_len(&s->out))
            dl = s->t_progress + (uint64_t)t->client_idle;
        else
            dl = s->t_state + (uint64_t)t->client_idle;
        break;
    case S_PROXY:
        if (s->up && s->up->connecting)
            dl = s->t_conn + (uint64_t)t->upstream_connect;
        else
            dl = s->t_progress + (uint64_t)t->upstream_read;
        break;
    case S_TUNNEL:
        dl = s->t_progress + (uint64_t)t->client_idle;
        break;
    case S_CLOSING:
        dl = s->t_state + CLOSING_MS;
        break;
    case S_LINGER:
    default:
        dl = s->t_state + LINGER_MS;
        break;
    }
    io_timer_set(W.loop, &s->timer, dl);
}

static void sess_timeout(io_timer_t *t)
{
    session_t *s = CONTAINER_OF(t, session_t, timer);
    if (s->closed)
        return;
    switch (s->st) {
    case S_HEAD:
        if (!s->head_timer) {
            sess_close(s);
            return;
        }
        send_error(s, 408);
        break;
    case S_PROXY:
        if (s->up && s->up->connecting) {
            upstream_failed(s, UF_CONNECT_TIMEOUT);
        } else if (!s->resp_head_done) {
            LOGW("timeout esperando respuesta de %s", s->log_server);
            send_error(s, 504);
        } else {
            sess_close(s);
            return;
        }
        break;
    default:
        sess_close(s);
        return;
    }
    if (!s->closed)
        sess_drive(s);
}

static void redrive(void *p)
{
    session_t *s = p;
    s->redrive = false;
    if (!s->closed)
        sess_drive(s);
}

static void sess_drive(session_t *s)
{
    for (int i = 0; i < DRIVE_BUDGET; i++) {
        bool p = false;
        if (s->st == S_HANDSHAKE) {
            p = do_handshake(s);
            if (s->closed)
                return;
            if (s->st == S_HANDSHAKE) {
                if (!p)
                    break;
                continue;
            }
        }
        p |= cli_read(s);
        if (s->closed)
            return;
        p |= process(s);
        if (s->closed)
            return;
        p |= up_io(s);
        if (s->closed)
            return;
        p |= process(s);
        if (s->closed)
            return;
        p |= cli_write(s);
        if (s->closed)
            return;
        if (s->st == S_CLOSING && (!s->out.data || !buf_len(&s->out))) {
            start_linger(s);
            if (s->closed)
                return;
            p = true;
        }
        if (!p) {
            update_timer(s);
            return;
        }
        s->t_progress = now_ms();
    }
    update_timer(s);
    if (!s->redrive && s->st != S_HANDSHAKE) {
        s->redrive = true;
        io_loop_defer(W.loop, redrive, s);
    }
}

static void cli_event(io_handler_t *h, uint32_t ev)
{
    session_t *s = (session_t *)h;
    if (s->closed)
        return;
    if (ev & (IO_READ | IO_HUP | IO_ERROR)) {
        s->rd = true;
        if (s->wr_needs_rd) {
            s->wr = true;
            s->wr_needs_rd = false;
        }
    }
    if (ev & (IO_WRITE | IO_HUP | IO_ERROR)) {
        s->wr = true;
        if (s->rd_needs_wr) {
            s->rd = true;
            s->rd_needs_wr = false;
        }
    }
    sess_drive(s);
}

void session_accept(int fd, const struct sockaddr *peer, const char *fe_key)
{
    frontend_t *fe = W.rt ? runtime_frontend(W.rt, fe_key) : NULL;
    session_t *s = fe ? sess_alloc() : NULL;
    if (!s) {
        close(fd);
        return;
    }
    s->h.on_event = cli_event;
    s->fd = fd;
    str_copy(s->fe_key, fe_key, sizeof(s->fe_key));
    addr_ip(peer, s->ip, sizeof(s->ip));
    set_nodelay(fd);
    io_timer_init(&s->timer, sess_timeout);
    s->t_accept = s->t_state = s->t_progress = now_ms();
    if (fe->tls) {
        s->ssl = tls_fe_new_ssl(fe->tls_fe, fd);
        if (!s->ssl) {
            close(fd);
            sess_free_deferred(s);
            return;
        }
        s->rt = W.rt; /* el callback SNI usa datos de esta generación */
        rt_ref(s->rt);
        s->st = S_HANDSHAKE;
    } else {
        s->st = S_HEAD;
    }
    if (io_loop_add(W.loop, fd, IO_READ | IO_WRITE, &s->h) < 0) {
        if (s->ssl)
            SSL_free(s->ssl);
        if (s->rt)
            rt_unref(s->rt);
        close(fd);
        sess_free_deferred(s);
        return;
    }
    s->next = W.sessions;
    if (W.sessions)
        W.sessions->prev = s;
    W.sessions = s;
    W.nsessions++;
    stat_inc(&W.st->accepted);
    atomic_fetch_add_explicit(&W.st->active_conns, 1, memory_order_relaxed);
    s->rd = s->wr = true; /* optimista: puede haber datos ya */
    sess_drive(s);
}

void sessions_close_idle(void)
{
    session_t *s = W.sessions;
    while (s) {
        session_t *n = s->next;
        if ((s->st == S_HEAD && (!s->in.data || !buf_len(&s->in)) &&
             (!s->out.data || !buf_len(&s->out))) ||
            s->st == S_HANDSHAKE)
            sess_close(s);
        s = n;
    }
}

void sessions_close_all(void)
{
    while (W.sessions)
        sess_close(W.sessions);
}
