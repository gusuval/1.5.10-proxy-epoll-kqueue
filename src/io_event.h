#ifndef IO_EVENT_H
#define IO_EVENT_H

/*
 * Abstracción del event loop sobre epoll (Linux) y kqueue (macOS/BSD).
 *
 * Todos los fds se registran en modo edge-triggered (EPOLLET / EV_CLEAR):
 * el loop avisa una vez por cambio de estado y el handler debe leer o
 * escribir hasta EAGAIN.
 *
 * Cada objeto registrado embebe un io_handler_t como primer miembro (o
 * donde quiera, usando CONTAINER_OF) y el loop llama a on_event.
 *
 * Incluye timers (min-heap con actualización perezosa) y callbacks
 * diferidos que se ejecutan al final de cada iteración, usados para
 * liberar objetos sin riesgo de eventos pendientes del mismo lote.
 */

#include <stdbool.h>
#include <stdint.h>

enum {
    IO_READ = 1u << 0,
    IO_WRITE = 1u << 1,
    IO_ERROR = 1u << 2,
    IO_HUP = 1u << 3,
};

typedef struct io_loop io_loop_t;

typedef struct io_handler {
    void (*on_event)(struct io_handler *h, uint32_t events);
} io_handler_t;

typedef struct io_timer {
    uint64_t deadline; /* deadline pedido (puede ser posterior a heap_key) */
    uint64_t heap_key; /* clave actual dentro del heap */
    int32_t idx;       /* posición en el heap, -1 si no está armado */
    void (*fn)(struct io_timer *t);
} io_timer_t;

io_loop_t *io_loop_create(int max_events);
int io_loop_add(io_loop_t *l, int fd, uint32_t events, io_handler_t *h);
int io_loop_mod(io_loop_t *l, int fd, uint32_t events, io_handler_t *h);
int io_loop_del(io_loop_t *l, int fd);
/* Ejecuta hasta io_loop_stop(). Devuelve 0 o -1 en error fatal. */
int io_loop_run(io_loop_t *l);
void io_loop_stop(io_loop_t *l);
void io_loop_destroy(io_loop_t *l);
/* Tras fork(): libera memoria sin tocar el estado del kernel compartido. */
void io_loop_abandon(io_loop_t *l);

/* Reloj monótono cacheado por iteración (ms). */
uint64_t io_loop_now(const io_loop_t *l);
void io_loop_update_time(io_loop_t *l);

void io_timer_init(io_timer_t *t, void (*fn)(io_timer_t *t));
/*
 * Arma el timer para 'deadline' (ms monótonos). Si ya estaba armado con
 * una clave anterior, el cambio es perezoso (O(1)): se reprograma al
 * vencer. Si el nuevo deadline es anterior, se reordena el heap.
 */
void io_timer_set(io_loop_t *l, io_timer_t *t, uint64_t deadline);
void io_timer_after(io_loop_t *l, io_timer_t *t, uint64_t ms);
void io_timer_cancel(io_loop_t *l, io_timer_t *t);
static inline bool io_timer_armed(const io_timer_t *t) { return t->idx >= 0; }

/* Ejecuta fn(arg) al final de la iteración actual del loop. */
void io_loop_defer(io_loop_t *l, void (*fn)(void *), void *arg);

#endif
