#ifndef IO_EVENT_INTERNAL_H
#define IO_EVENT_INTERNAL_H

#include "io_event.h"

typedef struct io_defer {
    void (*fn)(void *);
    void *arg;
} io_defer_t;

struct io_loop {
    int fd;
    int max_events;
    void *events;
    bool stop;
    uint64_t now;

    io_timer_t **heap;
    int nheap, capheap;

    io_defer_t *defer;
    int ndefer, capdefer;
};

/* Implementadas por io_event_epoll.c / io_event_kqueue.c */
int io_backend_init(io_loop_t *l);
void io_backend_close(io_loop_t *l);
/* Espera eventos y los despacha. 0 ok (incluye EINTR), -1 error. */
int io_backend_wait(io_loop_t *l, int timeout_ms);

#endif
