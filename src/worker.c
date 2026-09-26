#include "worker.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define TICK_MS 250
#define STOP_GRACE_MS 10000

worker_t W;

static int sig_pipe[2] = {-1, -1};

typedef struct {
    io_handler_t h;
    int fd;
} fd_handler_t;

static fd_handler_t chan_h, sig_h;
static io_timer_t tick;

/* ------------------------------------------------------------------ */
/* generaciones de configuración                                        */
/* ------------------------------------------------------------------ */

void rt_ref(runtime_t *rt) { rt->refcnt++; }

void rt_unref(runtime_t *rt)
{
    if (--rt->refcnt == 0) {
        rt->next = W.retired;
        W.retired = rt;
    }
}

static void destroy_runtime(runtime_t *rt)
{
    for (int i = 0; i < rt->nbe; i++)
        for (int j = 0; j < rt->bes[i].nservers; j++)
            upstream_pool_drain(&rt->bes[i].servers[j]);
    runtime_free(rt);
}

static void reap_retired(void)
{
    runtime_t **pp = &W.retired;
    while (*pp) {
        runtime_t *rt = *pp;
        if (health_done(rt->health)) {
            *pp = rt->next;
            LOGD("generación %llu liberada", (unsigned long long)rt->gen);
            destroy_runtime(rt);
        } else {
            pp = &rt->next;
        }
    }
}

/* ------------------------------------------------------------------ */
/* listeners                                                            */
/* ------------------------------------------------------------------ */

static void accept_retry(io_timer_t *t);

static void listener_event(io_handler_t *h, uint32_t ev)
{
    (void)ev;
    listener_t *l = (listener_t *)h;
    if (l->fd < 0)
        return;
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
#if defined(__linux__)
        int fd = accept4(l->fd, (struct sockaddr *)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
        int fd = accept(l->fd, (struct sockaddr *)&ss, &sl);
        if (fd >= 0) {
            set_nonblock(fd);
            set_cloexec(fd);
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        }
#endif
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                LOGE("accept en %s: %s (reintento en 100 ms)", l->key, strerror(errno));
                io_timer_after(W.loop, &l->retry, 100);
                break;
            }
            LOGE("accept en %s: %s", l->key, strerror(errno));
            break;
        }
        session_accept(fd, (struct sockaddr *)&ss, l->key);
    }
}

static void accept_retry(io_timer_t *t)
{
    listener_t *l = CONTAINER_OF(t, listener_t, retry);
    listener_event(&l->h, IO_READ);
}

static void listener_free_deferred(void *p) { free(p); }

static void listener_close(listener_t *l)
{
    io_timer_cancel(W.loop, &l->retry);
    if (l->fd >= 0) {
        io_loop_del(W.loop, l->fd);
        close(l->fd);
        l->fd = -1;
    }
    io_loop_defer(W.loop, listener_free_deferred, l);
}

static void close_all_listeners(void)
{
    listener_t *l = W.listeners;
    W.listeners = NULL;
    while (l) {
        listener_t *n = l->next;
        listener_close(l);
        l = n;
    }
}

/*
 * Sincroniza los listeners con los frontends de la nueva generación:
 * mantiene los existentes, abre los nuevos y cierra los eliminados.
 * keys/fds: sockets recibidos del master (macOS/BSD).
 */
static void apply_listeners(runtime_t *rt, char **keys, int *fds, int nfds)
{
    for (listener_t *l = W.listeners; l; l = l->next)
        l->keep = false;
    for (int i = 0; i < rt->nfe; i++) {
        const char *key = rt->fes[i].key;
        listener_t *l;
        for (l = W.listeners; l; l = l->next)
            if (strcmp(l->key, key) == 0)
                break;
        if (l) {
            l->keep = true;
            continue;
        }
        int fd = -1;
        char err[256] = "";
#if PROXY_PER_WORKER_LISTEN
        (void)keys;
        (void)fds;
        (void)nfds;
        fd = listener_open(key, true, err, sizeof(err));
#else
        for (int k = 0; k < nfds; k++)
            if (fds[k] >= 0 && strcmp(keys[k], key) == 0) {
                fd = fds[k];
                fds[k] = -1;
                break;
            }
        if (fd < 0)
            snprintf(err, sizeof(err), "el master no envió el socket");
#endif
        if (fd < 0) {
            LOGE("no se puede escuchar en %s (%s): %s", key, rt->fes[i].name, err);
            continue;
        }
        l = calloc(1, sizeof(*l));
        if (!l) {
            close(fd);
            continue;
        }
        l->h.on_event = listener_event;
        l->fd = fd;
        l->keep = true;
        str_copy(l->key, key, sizeof(l->key));
        io_timer_init(&l->retry, accept_retry);
        if (io_loop_add(W.loop, fd, IO_READ, &l->h) < 0) {
            close(fd);
            free(l);
            continue;
        }
        l->next = W.listeners;
        W.listeners = l;
        LOGI("escuchando en %s (%s%s)", key, rt->fes[i].name, rt->fes[i].tls ? ", tls" : "");
        /* Puede haber conexiones en cola desde antes del registro. */
        listener_event(&l->h, IO_READ);
    }
    listener_t **pp = &W.listeners;
    while (*pp) {
        listener_t *l = *pp;
        if (!l->keep) {
            *pp = l->next;
            LOGI("dejando de escuchar en %s", l->key);
            listener_close(l);
        } else {
            pp = &l->next;
        }
    }
#if !PROXY_PER_WORKER_LISTEN
    for (int k = 0; k < nfds; k++)
        if (fds[k] >= 0)
            close(fds[k]);
#endif
}

/* ------------------------------------------------------------------ */
/* configuración recibida del master                                    */
/* ------------------------------------------------------------------ */

/* payload: base_dir '\0' texto_toml '\0' claves separadas por '\n' */
static int handle_config(chan_msg_t *m)
{
    const char *base_dir = m->payload;
    size_t bl = strlen(base_dir);
    if (bl + 1 > m->len)
        return -1;
    const char *text = base_dir + bl + 1;
    size_t tl = strlen(text);
    char *keys_str = (bl + 1 + tl + 1 <= m->len) ? (char *)text + tl + 1 : NULL;
    char *keys[CHAN_MAX_FDS];
    int nk = 0;
    if (keys_str && *keys_str) {
        char *save = NULL;
        for (char *k = strtok_r(keys_str, "\n", &save); k && nk < CHAN_MAX_FDS;
             k = strtok_r(NULL, "\n", &save))
            keys[nk++] = k;
    }

    char err[512];
    config_t *cfg = config_parse(text, base_dir, err, sizeof(err));
    runtime_t *rt = cfg ? runtime_build(cfg, err, sizeof(err)) : NULL;
    if (!rt) {
        LOGE("configuración rechazada: %s", err);
        for (int i = 0; i < m->nfds; i++)
            close(m->fds[i]);
        return -1;
    }
    rt->gen = ++W.gen;
    runtime_start_health(rt);
    log_set_level(rt->cfg->log_level);
    apply_listeners(rt, keys, m->fds, MIN(nk, m->nfds));

    runtime_t *old = W.rt;
    W.rt = rt; /* swap: las peticiones nuevas ven la nueva generación */
    if (old) {
        health_stop(old->health);
        rt_unref(old);
        stat_inc(&W.st->reloads);
        LOGI("configuración aplicada (generación %llu)", (unsigned long long)rt->gen);
    }
    return 0;
}

static void begin_stop(void)
{
    if (W.stopping)
        return;
    W.stopping = true;
    W.stop_deadline = io_loop_now(W.loop) + STOP_GRACE_MS;
    LOGI("parada ordenada: %d conexiones abiertas", W.nsessions);
    close_all_listeners();
    sessions_close_idle();
}

static void chan_event(io_handler_t *h, uint32_t ev)
{
    (void)h;
    (void)ev;
    for (;;) {
        chan_msg_t m;
        int r = chan_recv(chan_h.fd, &m);
        if (r == 1)
            break;
        if (r < 0) {
            if (!W.stopping)
                LOGW("canal con el master cerrado");
            io_loop_del(W.loop, chan_h.fd);
            begin_stop();
            break;
        }
        if (m.cmd == CHAN_CONFIG) {
            if (handle_config(&m) < 0 && !W.rt) {
                LOGE("sin configuración válida, saliendo");
                free(m.payload);
                exit(2);
            }
        }
        free(m.payload);
    }
}

static void on_signal(int sig)
{
    int saved = errno;
    unsigned char c = (unsigned char)sig;
    ssize_t r = write(sig_pipe[1], &c, 1);
    (void)r;
    errno = saved;
}

static void sig_event(io_handler_t *h, uint32_t ev)
{
    (void)h;
    (void)ev;
    unsigned char c;
    while (read(sig_pipe[0], &c, 1) == 1)
        if (c == SIGTERM || c == SIGINT || c == SIGQUIT)
            begin_stop();
}

static void publish_stats(void)
{
    stats_worker_t *st = W.st;
    atomic_store_explicit(&st->log_dropped, log_dropped(), memory_order_relaxed);
    atomic_store_explicit(&st->buffers_in_use, bufpool_in_use(W.bp), memory_order_relaxed);
    if (!W.rt)
        return;
    stats_publish_begin(st);
    int n = 0;
    for (int i = 0; i < W.rt->nbe; i++) {
        backend_t *b = &W.rt->bes[i];
        for (int j = 0; j < b->nservers && n < STATS_MAX_SERVERS; j++) {
            server_t *s = &b->servers[j];
            stats_server_t *o = &st->servers[n++];
            str_copy(o->backend, b->name, sizeof(o->backend));
            str_copy(o->server, s->addr_str, sizeof(o->server));
            o->up = server_is_up(b, s, io_loop_now(W.loop));
            o->active = s->active;
            o->idle = s->idle_count;
            o->load_known = s->load_known && io_loop_now(W.loop) - s->load_ts <= LOAD_STALE_MS;
            o->load = s->load;
            o->requests = s->requests;
            o->failures = s->failures;
        }
    }
    st->nservers = n;
    stats_publish_end(st);
}

static void on_tick(io_timer_t *t)
{
    reap_retired();
    publish_stats();
    if (W.stopping) {
        if (W.nsessions == 0 || io_loop_now(W.loop) >= W.stop_deadline) {
            io_loop_stop(W.loop);
            return;
        }
        sessions_close_idle();
    }
    io_timer_after(W.loop, t, TICK_MS);
}

int worker_main(int id, int chan_fd, stats_worker_t *st)
{
    char tag[16];
    snprintf(tag, sizeof(tag), "w%d", id);
    log_reinit_child(tag);

    memset(&W, 0, sizeof(W));
    W.id = id;
    W.st = st;
    atomic_store(&st->pid, (int)getpid());
    atomic_store(&st->active_conns, 0);

    W.loop = io_loop_create(1024);
    W.bp = bufpool_create(256);
    if (!W.loop || !W.bp) {
        LOGE("no se puede crear el event loop");
        return 1;
    }
    if (pipe(sig_pipe) < 0)
        return 1;
    set_nonblock(sig_pipe[0]);
    set_nonblock(sig_pipe[1]);
    set_cloexec(sig_pipe[0]);
    set_cloexec(sig_pipe[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGHUP, SIG_IGN); /* la recarga la coordina el master */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    sig_h.h.on_event = sig_event;
    sig_h.fd = sig_pipe[0];
    io_loop_add(W.loop, sig_pipe[0], IO_READ, &sig_h.h);

    set_nonblock(chan_fd);
    chan_h.h.on_event = chan_event;
    chan_h.fd = chan_fd;
    io_loop_add(W.loop, chan_fd, IO_READ, &chan_h.h);
    chan_event(&chan_h.h, IO_READ); /* la config inicial puede estar ya */

    io_timer_init(&tick, on_tick);
    io_timer_after(W.loop, &tick, TICK_MS);

    LOGI("worker %d arrancado (pid %d)", id, (int)getpid());
    io_loop_run(W.loop);

    /* salida ordenada */
    close_all_listeners();
    sessions_close_all();
    if (W.rt) {
        health_stop(W.rt->health);
        rt_unref(W.rt);
        W.rt = NULL;
    }
    /* esperar a los hilos de health para liberar todas las generaciones */
    for (int i = 0; i < 200 && W.retired; i++) {
        reap_retired();
        if (W.retired)
            usleep(10000);
    }
    LOGI("worker %d terminado", id);
    atomic_store(&st->pid, 0);
    log_shutdown();
    return 0;
}
