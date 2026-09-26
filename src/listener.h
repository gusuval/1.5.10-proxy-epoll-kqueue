#ifndef LISTENER_H
#define LISTENER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io_event.h"

/* Linux reparte conexiones entre sockets SO_REUSEPORT; macOS/BSD no, así
 * que allí el master abre los sockets y los pasa a los workers. */
#if defined(__linux__)
#define PROXY_PER_WORKER_LISTEN 1
#else
#define PROXY_PER_WORKER_LISTEN 0
#endif

#define CHAN_MAGIC 0x50525859u
#define CHAN_MAX_FDS 64

enum { CHAN_CONFIG = 1 };

typedef struct listener {
    io_handler_t h;
    int fd;
    char key[128];
    bool keep;
    io_timer_t retry; /* reintento de accept tras EMFILE */
    struct listener *next;
} listener_t;

int listener_open(const char *addr, bool reuseport, char *err, size_t errlen);
/* Comprueba que la dirección se puede enlazar (sin quedarse el socket). */
int listener_test_bind(const char *addr, char *err, size_t errlen);

/* Canal master -> worker (socketpair) con paso de fds (SCM_RIGHTS). */
typedef struct {
    uint32_t cmd;
    char *payload; /* malloc, terminado en '\0' */
    uint32_t len;
    int fds[CHAN_MAX_FDS];
    int nfds;
} chan_msg_t;

int chan_send(int fd, uint32_t cmd, const void *payload, uint32_t len,
              const int *fds, int nfds);
/* 0 = mensaje recibido, 1 = no hay datos, -1 = EOF o error. */
int chan_recv(int fd, chan_msg_t *m);

#endif
