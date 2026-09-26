#include "io_event_internal.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

int io_backend_init(io_loop_t *l)
{
    l->fd = kqueue();
    if (l->fd < 0)
        return -1;
    l->events = calloc((size_t)l->max_events, sizeof(struct kevent));
    if (!l->events) {
        close(l->fd);
        return -1;
    }
    return 0;
}

void io_backend_close(io_loop_t *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
}

static int apply(io_loop_t *l, int fd, uint32_t events, io_handler_t *h,
                 bool adding)
{
    struct kevent ch[2];
    int n = 0;
    /* EV_CLEAR = edge-triggered. Con EV_ADD sobre un filtro existente se
     * actualiza; para quitar un filtro usamos EV_DISABLE. */
    EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ,
           (events & IO_READ) ? (EV_ADD | EV_ENABLE | EV_CLEAR)
                              : (adding ? (EV_ADD | EV_DISABLE | EV_CLEAR) : EV_DISABLE),
           0, 0, h);
    EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE,
           (events & IO_WRITE) ? (EV_ADD | EV_ENABLE | EV_CLEAR)
                               : (adding ? (EV_ADD | EV_DISABLE | EV_CLEAR) : EV_DISABLE),
           0, 0, h);
    return kevent(l->fd, ch, n, NULL, 0, NULL);
}

int io_loop_add(io_loop_t *l, int fd, uint32_t events, io_handler_t *h)
{
    return apply(l, fd, events, h, true);
}

int io_loop_mod(io_loop_t *l, int fd, uint32_t events, io_handler_t *h)
{
    return apply(l, fd, events, h, false);
}

int io_loop_del(io_loop_t *l, int fd)
{
    struct kevent ch[2];
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    /* Uno de los filtros puede no existir (ENOENT): se ignora. */
    kevent(l->fd, &ch[0], 1, NULL, 0, NULL);
    kevent(l->fd, &ch[1], 1, NULL, 0, NULL);
    return 0;
}

int io_backend_wait(io_loop_t *l, int timeout_ms)
{
    struct kevent *evs = l->events;
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int n = kevent(l->fd, NULL, 0, evs, l->max_events, tsp);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    /* reloj actualizado antes de despachar: los handlers calculan deadlines */
    l->now = mono_ms();
    for (int i = 0; i < n; i++) {
        uint32_t out = 0;
        if (evs[i].flags & EV_ERROR) {
            out |= IO_ERROR;
        } else if (evs[i].filter == EVFILT_READ) {
            out |= IO_READ;
            if (evs[i].flags & EV_EOF)
                out |= IO_HUP;
        } else if (evs[i].filter == EVFILT_WRITE) {
            out |= IO_WRITE;
            if (evs[i].flags & EV_EOF)
                out |= IO_HUP;
        }
        io_handler_t *h = evs[i].udata;
        if (h)
            h->on_event(h, out);
    }
    return 0;
}
