#include "io_event_internal.h"
#include "util.h"

#include <stdlib.h>
#include <unistd.h>

io_loop_t *io_loop_create(int max_events)
{
    io_loop_t *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->max_events = max_events > 0 ? max_events : 256;
    l->now = mono_ms();
    if (io_backend_init(l) < 0) {
        free(l);
        return NULL;
    }
    return l;
}

void io_loop_stop(io_loop_t *l) { l->stop = true; }

uint64_t io_loop_now(const io_loop_t *l) { return l->now; }

void io_loop_update_time(io_loop_t *l) { l->now = mono_ms(); }

static void free_mem(io_loop_t *l)
{
    free(l->events);
    free(l->heap);
    free(l->defer);
    free(l);
}

void io_loop_destroy(io_loop_t *l)
{
    if (!l)
        return;
    io_backend_close(l);
    free_mem(l);
}

void io_loop_abandon(io_loop_t *l)
{
    if (!l)
        return;
    /* Cerrar solo nuestro descriptor: nada de EPOLL_CTL_DEL, que alteraría
     * la instancia compartida con el proceso padre. */
    if (l->fd >= 0)
        close(l->fd);
    free_mem(l);
}

/* ---------------- timers: min-heap ---------------- */

static void heap_swap(io_loop_t *l, int a, int b)
{
    io_timer_t *t = l->heap[a];
    l->heap[a] = l->heap[b];
    l->heap[b] = t;
    l->heap[a]->idx = a;
    l->heap[b]->idx = b;
}

static void sift_up(io_loop_t *l, int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        if (l->heap[p]->heap_key <= l->heap[i]->heap_key)
            break;
        heap_swap(l, p, i);
        i = p;
    }
}

static void sift_down(io_loop_t *l, int i)
{
    for (;;) {
        int a = 2 * i + 1, b = a + 1, m = i;
        if (a < l->nheap && l->heap[a]->heap_key < l->heap[m]->heap_key)
            m = a;
        if (b < l->nheap && l->heap[b]->heap_key < l->heap[m]->heap_key)
            m = b;
        if (m == i)
            break;
        heap_swap(l, i, m);
        i = m;
    }
}

static void heap_push(io_loop_t *l, io_timer_t *t)
{
    if (l->nheap == l->capheap) {
        int nc = l->capheap ? l->capheap * 2 : 256;
        io_timer_t **nh = realloc(l->heap, (size_t)nc * sizeof(*nh));
        if (!nh)
            abort();
        l->heap = nh;
        l->capheap = nc;
    }
    t->idx = l->nheap;
    l->heap[l->nheap++] = t;
    sift_up(l, t->idx);
}

static void heap_remove(io_loop_t *l, io_timer_t *t)
{
    int i = t->idx;
    int last = --l->nheap;
    if (i != last) {
        l->heap[i] = l->heap[last];
        l->heap[i]->idx = i;
        sift_down(l, i);
        sift_up(l, i);
    }
    t->idx = -1;
}

void io_timer_init(io_timer_t *t, void (*fn)(io_timer_t *t))
{
    t->deadline = 0;
    t->heap_key = 0;
    t->idx = -1;
    t->fn = fn;
}

void io_timer_set(io_loop_t *l, io_timer_t *t, uint64_t deadline)
{
    t->deadline = deadline;
    if (t->idx >= 0) {
        if (deadline >= t->heap_key)
            return; /* perezoso: se reprograma al vencer */
        t->heap_key = deadline;
        sift_up(l, t->idx);
        return;
    }
    t->heap_key = deadline;
    heap_push(l, t);
}

void io_timer_after(io_loop_t *l, io_timer_t *t, uint64_t ms)
{
    io_timer_set(l, t, l->now + ms);
}

void io_timer_cancel(io_loop_t *l, io_timer_t *t)
{
    if (t->idx >= 0)
        heap_remove(l, t);
}

static void run_timers(io_loop_t *l)
{
    while (l->nheap && l->heap[0]->heap_key <= l->now) {
        io_timer_t *t = l->heap[0];
        heap_remove(l, t);
        if (t->deadline > l->now) {
            t->heap_key = t->deadline;
            heap_push(l, t);
            continue;
        }
        t->fn(t);
    }
}

/* ---------------- defer ---------------- */

void io_loop_defer(io_loop_t *l, void (*fn)(void *), void *arg)
{
    if (l->ndefer == l->capdefer) {
        int nc = l->capdefer ? l->capdefer * 2 : 64;
        io_defer_t *nd = realloc(l->defer, (size_t)nc * sizeof(*nd));
        if (!nd)
            abort();
        l->defer = nd;
        l->capdefer = nc;
    }
    l->defer[l->ndefer].fn = fn;
    l->defer[l->ndefer].arg = arg;
    l->ndefer++;
}

static void run_defers(io_loop_t *l)
{
    /* Solo se ejecutan los pendientes al empezar; los que se añadan ahora
     * quedan para la siguiente iteración (FIFO). */
    int n = l->ndefer;
    if (!n)
        return;
    io_defer_t *batch = l->defer;
    l->defer = NULL;
    l->ndefer = 0;
    l->capdefer = 0;
    for (int i = 0; i < n; i++)
        batch[i].fn(batch[i].arg);
    free(batch);
}

int io_loop_run(io_loop_t *l)
{
    l->stop = false;
    while (!l->stop) {
        int timeout = -1;
        l->now = mono_ms();
        if (l->ndefer) {
            timeout = 0;
        } else if (l->nheap) {
            uint64_t k = l->heap[0]->heap_key;
            timeout = k <= l->now ? 0 : (int)MIN(k - l->now, 60000u);
        }
        if (io_backend_wait(l, timeout) < 0)
            return -1;
        l->now = mono_ms();
        run_timers(l);
        run_defers(l);
    }
    return 0;
}
