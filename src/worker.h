#ifndef WORKER_H
#define WORKER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "buffer_pool.h"
#include "io_event.h"
#include "listener.h"
#include "runtime.h"
#include "stats.h"

struct session;

/* Estado global del proceso worker (un único hilo de event loop). */
typedef struct worker {
    int id;
    io_loop_t *loop;
    bufpool_t *bp;
    stats_worker_t *st;
    runtime_t *rt;      /* generación actual */
    runtime_t *retired; /* pendientes de liberar */
    uint64_t gen;
    listener_t *listeners;
    struct session *sessions;
    int nsessions;
    bool stopping;
    uint64_t stop_deadline;
} worker_t;

extern worker_t W;

void rt_ref(runtime_t *rt);
void rt_unref(runtime_t *rt);

int worker_main(int id, int chan_fd, stats_worker_t *st);

/* connection.c */
void session_accept(int fd, const struct sockaddr *peer, const char *fe_key);
void sessions_close_idle(void);
void sessions_close_all(void);
void upstream_pool_drain(server_t *srv);

#endif
