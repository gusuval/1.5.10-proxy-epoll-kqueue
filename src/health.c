#include "health.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    backend_t *be;
    server_t *srv;
    uint64_t next;
} target_t;

struct health {
    target_t *t;
    int n;
    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    atomic_bool done;
};

static int wait_fd(int fd, short ev, int timeout_ms)
{
    struct pollfd p = {.fd = fd, .events = ev};
    int r;
    do {
        r = poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return r;
}

bool health_probe(const server_t *s, const cfg_health_t *cfg)
{
    int fd = socket(s->sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    set_nonblock(fd);
    set_cloexec(fd);
    bool ok = false;
    int r = connect(fd, (const struct sockaddr *)&s->sa, s->salen);
    if (r < 0 && errno != EINPROGRESS)
        goto out;
    if (r < 0) {
        if (wait_fd(fd, POLLOUT, cfg->timeout_ms) <= 0)
            goto out;
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err)
            goto out;
    }
    if (cfg->type == HEALTH_TCP) {
        ok = true;
        goto out;
    }
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: proxy-health\r\n"
                     "Connection: close\r\n\r\n",
                     cfg->path, s->addr_str);
    if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n)
        goto out;
    char resp[64];
    size_t got = 0;
    while (got < 12) {
        if (wait_fd(fd, POLLIN, cfg->timeout_ms) <= 0)
            goto out;
        ssize_t k = recv(fd, resp + got, sizeof(resp) - got, 0);
        if (k <= 0)
            goto out;
        got += (size_t)k;
    }
    ok = memcmp(resp, "HTTP/1.", 7) == 0 && resp[9] == '2';
out:
    close(fd);
    return ok;
}

static void *run(void *arg)
{
    health_t *h = arg;
    pthread_mutex_lock(&h->mu);
    while (!h->stop) {
        uint64_t now = mono_ms();
        uint64_t next = now + 1000;
        for (int i = 0; i < h->n && !h->stop; i++) {
            target_t *t = &h->t[i];
            if (t->next <= now) {
                pthread_mutex_unlock(&h->mu);
                bool ok = health_probe(t->srv, &t->be->health);
                pthread_mutex_lock(&h->mu);
                if (h->stop)
                    break;
                if (server_probe_result(t->be, t->srv, ok))
                    LOGW("backend '%s' servidor %s -> %s (sonda %s)", t->be->name,
                         t->srv->addr_str, ok ? "UP" : "DOWN",
                         t->be->health.type == HEALTH_HTTP ? "http" : "tcp");
                now = mono_ms();
                t->next = now + (uint64_t)t->be->health.interval_ms;
            }
            if (t->next < next)
                next = t->next;
        }
        if (h->stop)
            break;
        now = mono_ms();
        if (next > now) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t ns = (uint64_t)ts.tv_nsec + (next - now) * 1000000ULL;
            ts.tv_sec += (time_t)(ns / 1000000000ULL);
            ts.tv_nsec = (long)(ns % 1000000000ULL);
            pthread_cond_timedwait(&h->cv, &h->mu, &ts);
        }
    }
    pthread_mutex_unlock(&h->mu);
    atomic_store(&h->done, true);
    return NULL;
}

health_t *health_start(backend_t *bes, int nbe)
{
    int n = 0;
    for (int i = 0; i < nbe; i++)
        if (bes[i].health.type != HEALTH_NONE)
            n += bes[i].nservers;
    if (n == 0)
        return NULL;
    health_t *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->t = calloc((size_t)n, sizeof(target_t));
    if (!h->t) {
        free(h);
        return NULL;
    }
    uint64_t now = mono_ms();
    for (int i = 0; i < nbe; i++) {
        if (bes[i].health.type == HEALTH_NONE)
            continue;
        for (int j = 0; j < bes[i].nservers; j++)
            h->t[h->n++] = (target_t){.be = &bes[i], .srv = &bes[i].servers[j], .next = now};
    }
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    atomic_init(&h->done, false);
    if (pthread_create(&h->thr, NULL, run, h) != 0) {
        free(h->t);
        free(h);
        return NULL;
    }
    pthread_detach(h->thr);
    return h;
}

void health_stop(health_t *h)
{
    if (!h)
        return;
    pthread_mutex_lock(&h->mu);
    h->stop = true;
    pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

bool health_done(health_t *h) { return !h || atomic_load(&h->done); }

void health_free(health_t *h)
{
    if (!h)
        return;
    pthread_mutex_destroy(&h->mu);
    pthread_cond_destroy(&h->cv);
    free(h->t);
    free(h);
}
