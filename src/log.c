#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------- ring MPSC (Vyukov bounded queue) ---------------- */

#define CELL_PAYLOAD (LOG_SLOT_SIZE - sizeof(atomic_size_t) - sizeof(uint32_t))

typedef struct {
    atomic_size_t seq;
    uint32_t len;
    char data[CELL_PAYLOAD];
} cell_t;

struct log_ring {
    size_t mask;
    cell_t *cells;
    _Alignas(64) atomic_size_t head; /* productores */
    _Alignas(64) size_t tail;        /* consumidor único */
};

log_ring_t *log_ring_new(size_t slots)
{
    /* slots debe ser potencia de 2 */
    log_ring_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->cells = calloc(slots, sizeof(cell_t));
    if (!r->cells) {
        free(r);
        return NULL;
    }
    r->mask = slots - 1;
    for (size_t i = 0; i < slots; i++)
        atomic_init(&r->cells[i].seq, i);
    atomic_init(&r->head, 0);
    r->tail = 0;
    return r;
}

void log_ring_free(log_ring_t *r)
{
    if (!r)
        return;
    free(r->cells);
    free(r);
}

bool log_ring_push(log_ring_t *r, const char *msg, size_t len)
{
    size_t pos = atomic_load_explicit(&r->head, memory_order_relaxed);
    cell_t *c;
    for (;;) {
        c = &r->cells[pos & r->mask];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->head, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
        } else if (diff < 0) {
            return false; /* lleno */
        } else {
            pos = atomic_load_explicit(&r->head, memory_order_relaxed);
        }
    }
    if (len > CELL_PAYLOAD)
        len = CELL_PAYLOAD;
    memcpy(c->data, msg, len);
    c->len = (uint32_t)len;
    atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
    return true;
}

int log_ring_pop(log_ring_t *r, char *out, size_t outlen)
{
    cell_t *c = &r->cells[r->tail & r->mask];
    size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
    if (seq != r->tail + 1)
        return -1;
    size_t n = c->len < outlen ? c->len : outlen;
    memcpy(out, c->data, n);
    atomic_store_explicit(&c->seq, r->tail + r->mask + 1, memory_order_release);
    r->tail++;
    return (int)n;
}

/* ---------------- logger del proceso ---------------- */

static struct {
    log_ring_t *ring;
    int fd;
    bool own_fd;
    atomic_int level;
    atomic_bool stop;
    atomic_uint_fast64_t dropped;
    pthread_t thr;
    bool running;
    char tag[16];
    char path[512];
} G = {.fd = 2};

static const char *level_name[] = {"debug", "info", "warn", "error", "access"};

int log_level_from_str(const char *s)
{
    for (int i = 0; i <= LOG_ERROR; i++)
        if (strcmp(s, level_name[i]) == 0)
            return i;
    return -1;
}

void log_set_level(int level) { atomic_store(&G.level, level); }

uint64_t log_dropped(void) { return atomic_load(&G.dropped); }

static void *consumer(void *arg)
{
    (void)arg;
    char out[65536];
    size_t n = 0;
    for (;;) {
        bool got = false;
        for (;;) {
            if (n + LOG_SLOT_SIZE + 1 > sizeof(out)) {
                write_all(G.fd, out, n);
                n = 0;
            }
            int k = log_ring_pop(G.ring, out + n, sizeof(out) - n - 1);
            if (k < 0)
                break;
            n += (size_t)k;
            got = true;
        }
        if (n) {
            write_all(G.fd, out, n);
            n = 0;
        }
        if (!got) {
            if (atomic_load(&G.stop))
                break;
            struct timespec ts = {0, 5 * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static int start(void)
{
    G.ring = log_ring_new(LOG_RING_SLOTS);
    if (!G.ring)
        return -1;
    atomic_store(&G.stop, false);
    if (pthread_create(&G.thr, NULL, consumer, NULL) != 0)
        return -1;
    G.running = true;
    return 0;
}

int log_init(const char *path, int level, const char *tag)
{
    atomic_store(&G.level, level);
    str_copy(G.tag, tag ? tag : "", sizeof(G.tag));
    if (path && path[0] && strcmp(path, "-") != 0) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            fprintf(stderr, "no se puede abrir log '%s': %s\n", path,
                    strerror(errno));
            return -1;
        }
        G.fd = fd;
        G.own_fd = true;
        str_copy(G.path, path, sizeof(G.path));
    } else {
        G.fd = 2;
        G.own_fd = false;
    }
    return start();
}

int log_reinit_child(const char *tag)
{
    /* El hilo consumidor no existe en el hijo; el ring heredado puede
     * contener mensajes del padre que no deben duplicarse. */
    G.running = false;
    G.ring = NULL; /* memoria heredada: se abandona */
    str_copy(G.tag, tag ? tag : "", sizeof(G.tag));
    return start();
}

void log_shutdown(void)
{
    if (!G.running)
        return;
    atomic_store(&G.stop, true);
    pthread_join(G.thr, NULL);
    G.running = false;
    log_ring_free(G.ring);
    G.ring = NULL;
}

void log_vmsg(int level, const char *fmt, va_list ap)
{
    if (level < atomic_load_explicit(&G.level, memory_order_relaxed))
        return;
    char line[LOG_SLOT_SIZE];
    static _Thread_local time_t cached_sec = -1;
    static _Thread_local char cached_ts[32];

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (ts.tv_sec != cached_sec) {
        struct tm tm;
        gmtime_r(&ts.tv_sec, &tm);
        strftime(cached_ts, sizeof(cached_ts), "%Y-%m-%dT%H:%M:%S", &tm);
        cached_sec = ts.tv_sec;
    }
    int n = snprintf(line, sizeof(line), "%s.%03ldZ [%s] [%s] ", cached_ts,
                     ts.tv_nsec / 1000000L, level_name[level], G.tag);
    if (n < 0)
        return;
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    if (m < 0)
        return;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(line) - 2)
        len = sizeof(line) - 2;
    line[len++] = '\n';

    if (!G.running || !G.ring) {
        write_all(G.fd, line, len);
        return;
    }
    if (!log_ring_push(G.ring, line, len))
        atomic_fetch_add_explicit(&G.dropped, 1, memory_order_relaxed);
}

void log_msg(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(level, fmt, ap);
    va_end(ap);
}
