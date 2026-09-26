/* buffer_pool, timers del event loop y ring buffer del log */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "buffer_pool.h"
#include "io_event.h"
#include "log.h"
#include "util.h"

static void test_bufpool(void **st)
{
    (void)st;
    bufpool_t *p = bufpool_create(4);
    char *s[10];
    for (int i = 0; i < 10; i++) {
        s[i] = bufpool_get(p);
        assert_non_null(s[i]);
        memset(s[i], i, BUF_SIZE); /* el slot entero es usable */
    }
    assert_int_equal(bufpool_in_use(p), 10);
    assert_int_equal(bufpool_allocated(p), 12);
    for (int i = 0; i < 10; i++)
        for (int j = i + 1; j < 10; j++)
            assert_true(s[i] != s[j]);
    bufpool_put(p, s[3]);
    assert_ptr_equal(bufpool_get(p), s[3]); /* LIFO */
    for (int i = 0; i < 10; i++)
        bufpool_put(p, s[i]);
    assert_int_equal(bufpool_in_use(p), 0);
    bufpool_destroy(p);
}

static void test_buf_ops(void **st)
{
    (void)st;
    bufpool_t *p = bufpool_create(2);
    buf_t a = {0}, b = {0};
    assert_true(buf_ensure(p, &a));
    assert_true(buf_ensure(p, &b));
    memcpy(buf_wptr(&a), "hello world", 11);
    a.w += 11;
    buf_consume(&a, 6);
    assert_int_equal(buf_len(&a), 5);
    buf_compact(&a);
    assert_int_equal(a.r, 0);
    assert_memory_equal(buf_rptr(&a), "world", 5);
    assert_int_equal(buf_move(&b, &a), 5);
    assert_int_equal(buf_len(&a), 0);
    assert_int_equal(a.r, 0); /* vacío => reinicia posiciones */
    assert_memory_equal(buf_rptr(&b), "world", 5);
    buf_release(p, &a);
    buf_release(p, &b);
    assert_int_equal(bufpool_in_use(p), 0);
    bufpool_destroy(p);
}

static int fired[8], nfired;
static io_loop_t *L;

typedef struct {
    io_timer_t t;
    int id;
} tt_t;

static void on_tt(io_timer_t *t)
{
    tt_t *x = CONTAINER_OF(t, tt_t, t);
    fired[nfired++] = x->id;
    if (nfired == 3)
        io_loop_stop(L);
}

static void test_timers_order_cancel_lazy(void **st)
{
    (void)st;
    L = io_loop_create(16);
    tt_t a = {.id = 1}, b = {.id = 2}, c = {.id = 3}, d = {.id = 4};
    io_timer_init(&a.t, on_tt);
    io_timer_init(&b.t, on_tt);
    io_timer_init(&c.t, on_tt);
    io_timer_init(&d.t, on_tt);
    io_loop_update_time(L);
    io_timer_after(L, &c.t, 30);
    io_timer_after(L, &a.t, 10);
    io_timer_after(L, &b.t, 20);
    io_timer_after(L, &d.t, 15);
    io_timer_cancel(L, &d.t);
    assert_false(io_timer_armed(&d.t));
    /* aplazar a 'a' (perezoso) debe hacerlo disparar después de 'b' */
    io_timer_after(L, &a.t, 25);
    nfired = 0;
    uint64_t t0 = mono_ms();
    io_loop_run(L);
    uint64_t el = mono_ms() - t0;
    assert_int_equal(nfired, 3);
    assert_int_equal(fired[0], 2);
    assert_int_equal(fired[1], 1);
    assert_int_equal(fired[2], 3);
    assert_true(el >= 29);
    io_loop_destroy(L);
}

typedef struct {
    io_handler_t h;
    int fd;
    int reads;
} pipe_h_t;

static void on_pipe(io_handler_t *h, uint32_t ev)
{
    pipe_h_t *p = (pipe_h_t *)h;
    if (ev & IO_READ) {
        char buf[64];
        while (read(p->fd, buf, sizeof(buf)) > 0)
            p->reads++;
        io_loop_stop(L);
    }
}

static uint64_t seen_now;

static void on_pipe_now(io_handler_t *h, uint32_t ev)
{
    (void)h;
    (void)ev;
    seen_now = io_loop_now(L);
    io_loop_stop(L);
}

static void *late_writer(void *arg)
{
    usleep(200 * 1000);
    ssize_t r = write((int)(intptr_t)arg, "x", 1);
    (void)r;
    return NULL;
}

/* Regresión: el handler debe ver el reloj de después de epoll_wait/kevent. */
static void test_now_fresh_in_handler(void **st)
{
    (void)st;
    L = io_loop_create(16);
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    pipe_h_t ph = {.h.on_event = on_pipe_now, .fd = fds[0]};
    io_loop_add(L, fds[0], IO_READ, &ph.h);
    pthread_t th;
    pthread_create(&th, NULL, late_writer, (void *)(intptr_t)fds[1]);
    uint64_t t0 = mono_ms();
    io_loop_run(L);
    pthread_join(th, NULL);
    assert_true(seen_now >= t0 + 190);
    close(fds[0]);
    close(fds[1]);
    io_loop_destroy(L);
}

static void test_io_readiness(void **st)
{
    (void)st;
    L = io_loop_create(16);
    int fds[2];
    assert_int_equal(pipe(fds), 0);
    set_nonblock(fds[0]);
    pipe_h_t ph = {.h.on_event = on_pipe, .fd = fds[0]};
    assert_int_equal(io_loop_add(L, fds[0], IO_READ, &ph.h), 0);
    assert_int_equal(write(fds[1], "x", 1), 1);
    io_loop_run(L);
    assert_true(ph.reads >= 1);
    io_loop_del(L, fds[0]);
    close(fds[0]);
    close(fds[1]);
    io_loop_destroy(L);
}

static void test_log_ring_basic(void **st)
{
    (void)st;
    log_ring_t *r = log_ring_new(4);
    char out[600];
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), -1);
    assert_true(log_ring_push(r, "a", 1));
    assert_true(log_ring_push(r, "bb", 2));
    assert_true(log_ring_push(r, "ccc", 3));
    assert_true(log_ring_push(r, "dddd", 4));
    assert_false(log_ring_push(r, "e", 1)); /* lleno: se descarta */
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), 1);
    assert_true(log_ring_push(r, "e", 1));
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), 2);
    assert_memory_equal(out, "bb", 2);
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), 3);
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), 4);
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), 1);
    assert_memory_equal(out, "e", 1);
    assert_int_equal(log_ring_pop(r, out, sizeof(out)), -1);
    log_ring_free(r);
}

#define NPROD 4
#define PER 20000
static log_ring_t *R;

static void *producer(void *arg)
{
    int id = (int)(intptr_t)arg;
    char m[32];
    for (int i = 0; i < PER; i++) {
        int n = snprintf(m, sizeof(m), "%d:%d", id, i);
        while (!log_ring_push(R, m, (size_t)n))
            ; /* en el test se reintenta; el logger real descarta */
    }
    return NULL;
}

static void test_log_ring_concurrent(void **st)
{
    (void)st;
    R = log_ring_new(256);
    pthread_t th[NPROD];
    for (int i = 0; i < NPROD; i++)
        pthread_create(&th[i], NULL, producer, (void *)(intptr_t)i);
    int last[NPROD];
    for (int i = 0; i < NPROD; i++)
        last[i] = -1;
    int got = 0;
    char out[64];
    while (got < NPROD * PER) {
        int n = log_ring_pop(R, out, sizeof(out) - 1);
        if (n < 0)
            continue;
        out[n] = 0;
        int id, seq;
        assert_int_equal(sscanf(out, "%d:%d", &id, &seq), 2);
        assert_true(seq > last[id]); /* FIFO por productor */
        last[id] = seq;
        got++;
    }
    for (int i = 0; i < NPROD; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < NPROD; i++)
        assert_int_equal(last[i], PER - 1);
    log_ring_free(R);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bufpool),
        cmocka_unit_test(test_buf_ops),
        cmocka_unit_test(test_timers_order_cancel_lazy),
        cmocka_unit_test(test_io_readiness),
        cmocka_unit_test(test_now_fresh_in_handler),
        cmocka_unit_test(test_log_ring_basic),
        cmocka_unit_test(test_log_ring_concurrent),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
