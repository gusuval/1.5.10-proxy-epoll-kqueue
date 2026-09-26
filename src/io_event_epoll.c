#include "io_event_internal.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

int io_backend_init(io_loop_t *l)
{
    l->fd = epoll_create1(EPOLL_CLOEXEC);
    if (l->fd < 0)
        return -1;
    l->events = calloc((size_t)l->max_events, sizeof(struct epoll_event));
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

static uint32_t to_epoll(uint32_t ev)
{
    uint32_t e = EPOLLET | EPOLLRDHUP;
    if (ev & IO_READ)
        e |= EPOLLIN;
    if (ev & IO_WRITE)
        e |= EPOLLOUT;
    return e;
}

int io_loop_add(io_loop_t *l, int fd, uint32_t events, io_handler_t *h)
{
    struct epoll_event e = {.events = to_epoll(events), .data.ptr = h};
    return epoll_ctl(l->fd, EPOLL_CTL_ADD, fd, &e);
}

int io_loop_mod(io_loop_t *l, int fd, uint32_t events, io_handler_t *h)
{
    struct epoll_event e = {.events = to_epoll(events), .data.ptr = h};
    return epoll_ctl(l->fd, EPOLL_CTL_MOD, fd, &e);
}

int io_loop_del(io_loop_t *l, int fd)
{
    struct epoll_event e = {0};
    return epoll_ctl(l->fd, EPOLL_CTL_DEL, fd, &e);
}

int io_backend_wait(io_loop_t *l, int timeout_ms)
{
    struct epoll_event *evs = l->events;
    int n = epoll_wait(l->fd, evs, l->max_events, timeout_ms);
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    /* reloj actualizado antes de despachar: los handlers calculan deadlines */
    l->now = mono_ms();
    for (int i = 0; i < n; i++) {
        uint32_t e = evs[i].events, out = 0;
        if (e & EPOLLIN)
            out |= IO_READ;
        if (e & EPOLLOUT)
            out |= IO_WRITE;
        if (e & EPOLLERR)
            out |= IO_ERROR;
        if (e & (EPOLLHUP | EPOLLRDHUP))
            out |= IO_HUP;
        io_handler_t *h = evs[i].data.ptr;
        h->on_event(h, out);
    }
    return 0;
}
