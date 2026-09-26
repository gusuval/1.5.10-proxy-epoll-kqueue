#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "backend_pool.h"

static cfg_server_t SRV[4];

static void mk(backend_t *b, strategy_t s, int n, const int *weights)
{
    cfg_backend_t c;
    memset(&c, 0, sizeof(c));
    strcpy(c.name, "t");
    c.strategy = s;
    c.nservers = n;
    c.servers = SRV;
    c.health.type = HEALTH_NONE;
    c.health.fall = 2;
    c.health.rise = 2;
    c.health.interval_ms = 1000;
    for (int i = 0; i < n; i++) {
        snprintf(SRV[i].addr, sizeof(SRV[i].addr), "127.0.0.1:%d", 9000 + i);
        SRV[i].weight = weights ? weights[i] : 1;
    }
    char err[128];
    assert_int_equal(backend_init(b, &c, err, sizeof(err)), 0);
}

static void test_round_robin(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_ROUND_ROBIN, 3, NULL);
    int count[3] = {0};
    for (int i = 0; i < 300; i++)
        count[backend_pick(&b, 0, 1000)->idx]++;
    for (int i = 0; i < 3; i++)
        assert_int_equal(count[i], 100);
    /* orden estricto */
    int a = backend_pick(&b, 0, 1000)->idx;
    assert_int_equal(backend_pick(&b, 0, 1000)->idx, (a + 1) % 3);
    backend_fini(&b);
}

static void test_weighted_smooth(void **st)
{
    (void)st;
    backend_t b;
    int w[] = {3, 1};
    mk(&b, STRAT_WEIGHTED, 2, w);
    int count[2] = {0}, run = 0, maxrun = 0, last = -1;
    for (int i = 0; i < 400; i++) {
        int k = backend_pick(&b, 0, 1000)->idx;
        count[k]++;
        run = k == last ? run + 1 : 1;
        last = k;
        if (run > maxrun)
            maxrun = run;
    }
    assert_int_equal(count[0], 300);
    assert_int_equal(count[1], 100);
    assert_true(maxrun <= 3); /* sin ráfagas largas del servidor pesado */
    backend_fini(&b);
}

static void test_least_conn(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_LEAST_CONN, 3, NULL);
    b.servers[0].active = 5;
    b.servers[1].active = 1;
    b.servers[2].active = 3;
    assert_int_equal(backend_pick(&b, 0, 1000)->idx, 1);
    b.servers[1].active = 9;
    assert_int_equal(backend_pick(&b, 0, 1000)->idx, 2);
    /* empate: rotación */
    b.servers[0].active = b.servers[1].active = b.servers[2].active = 0;
    int a = backend_pick(&b, 0, 1000)->idx, c = backend_pick(&b, 0, 1000)->idx;
    assert_int_not_equal(a, c);
    backend_fini(&b);
}

static void test_least_load(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_LEAST_LOAD, 2, NULL);
    server_report_load(&b.servers[0], 0.1, 1000);
    server_report_load(&b.servers[1], 0.9, 1000);
    for (int i = 0; i < 50; i++)
        assert_int_equal(backend_pick(&b, 0, 1000)->idx, 0);
    /* EMA: una muestra alta no invierte de golpe */
    server_report_load(&b.servers[0], 1.0, 1100);
    assert_true(b.servers[0].load > 0.35 && b.servers[0].load < 0.45);
    backend_fini(&b);
}

static void test_least_load_stale(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_LEAST_LOAD, 2, NULL);
    server_report_load(&b.servers[0], 0.9, 1000);
    server_report_load(&b.servers[1], 0.1, 1000);
    b.servers[0].active = 0;
    b.servers[1].active = 10;
    /* pasados 5 s la carga caduca: se decide por conexiones activas */
    uint64_t later = 1000 + LOAD_STALE_MS + 1;
    for (int i = 0; i < 20; i++)
        assert_int_equal(backend_pick(&b, 0, later)->idx, 0);
    /* una muestra nueva tras caducar reemplaza (no promedia) */
    server_report_load(&b.servers[0], 0.2, later);
    assert_true(b.servers[0].load > 0.19 && b.servers[0].load < 0.21);
    backend_fini(&b);
}

static void test_least_load_explores_unknown(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_LEAST_LOAD, 2, NULL);
    server_report_load(&b.servers[1], 0.9, 1000);
    /* s0 sin carga conocida y mismas conexiones activas: se explora s0 */
    for (int i = 0; i < 20; i++)
        assert_int_equal(backend_pick(&b, 0, 1000)->idx, 0);
    /* pero least_conn manda sobre la exploración */
    b.servers[0].active = 3;
    for (int i = 0; i < 20; i++)
        assert_int_equal(backend_pick(&b, 0, 1000)->idx, 1);
    backend_fini(&b);
}

static void test_down_excluded(void **st)
{
    (void)st;
    strategy_t all[] = {STRAT_ROUND_ROBIN, STRAT_WEIGHTED, STRAT_LEAST_CONN, STRAT_LEAST_LOAD};
    for (size_t s = 0; s < 4; s++) {
        backend_t b;
        mk(&b, all[s], 3, NULL);
        b.health.type = HEALTH_TCP; /* sin recuperación pasiva */
        atomic_store(&b.servers[1].up, 0);
        for (int i = 0; i < 60; i++)
            assert_int_not_equal(backend_pick(&b, 0, 1000)->idx, 1);
        atomic_store(&b.servers[0].up, 0);
        atomic_store(&b.servers[2].up, 0);
        assert_null(backend_pick(&b, 0, 1000));
        backend_fini(&b);
    }
}

static void test_exclude_mask(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_ROUND_ROBIN, 3, NULL);
    for (int i = 0; i < 10; i++)
        assert_int_equal(backend_pick(&b, 0x3, 1000)->idx, 2);
    assert_null(backend_pick(&b, 0x7, 1000));
    backend_fini(&b);
}

static void test_passive_health(void **st)
{
    (void)st;
    backend_t b;
    mk(&b, STRAT_ROUND_ROBIN, 2, NULL);
    server_t *s = &b.servers[0];
    assert_false(server_passive_fail(&b, s, 1000));
    assert_true(server_passive_fail(&b, s, 1000)); /* fall = 2 */
    assert_false(server_is_up(&b, s, 1500));
    /* sin health activo: se reintenta tras interval */
    assert_true(server_is_up(&b, s, 2001));
    assert_true(server_passive_fail(&b, s, 2002)); /* half-open: cae al primer fallo */
    /* sonda activa: rise = 2 */
    b.health.type = HEALTH_HTTP;
    atomic_store(&s->up, 0);
    assert_false(server_probe_result(&b, s, true));
    assert_true(server_probe_result(&b, s, true));
    assert_true(atomic_load(&s->up));
    backend_fini(&b);
}

static void test_parse_load(void **st)
{
    (void)st;
    double v;
    assert_true(parse_load("0.5", 3, &v));
    assert_true(v > 0.49 && v < 0.51);
    assert_true(parse_load(" 1 ", 3, &v));
    assert_false(parse_load("1.5", 3, &v));
    assert_false(parse_load("-0.1", 4, &v));
    assert_false(parse_load("abc", 3, &v));
    assert_false(parse_load("", 0, &v));
    assert_false(parse_load("nan", 3, &v));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_round_robin),   cmocka_unit_test(test_weighted_smooth),
        cmocka_unit_test(test_least_conn),    cmocka_unit_test(test_least_load),
        cmocka_unit_test(test_least_load_stale), cmocka_unit_test(test_least_load_explores_unknown),
        cmocka_unit_test(test_down_excluded),
        cmocka_unit_test(test_exclude_mask),  cmocka_unit_test(test_passive_health),
        cmocka_unit_test(test_parse_load),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
