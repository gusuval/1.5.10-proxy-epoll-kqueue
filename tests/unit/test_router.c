#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "router.h"
#include "tls.h"

#define LOOK(r, h) router_lookup((r), (h), strlen(h))

static void test_exact_wildcard_default(void **st)
{
    (void)st;
    router_t *r = router_new();
    assert_int_equal(router_add(r, "api.test", 1), 0);
    assert_int_equal(router_add(r, "*.example.com", 2), 0);
    assert_int_equal(router_add(r, "*.eu.example.com", 3), 0);
    assert_int_equal(router_add(r, "www.example.com", 4), 0);
    router_build(r);

    assert_int_equal(LOOK(r, "api.test"), 1);
    assert_int_equal(LOOK(r, "API.Test"), 1);
    assert_int_equal(LOOK(r, "a.example.com"), 2);
    assert_int_equal(LOOK(r, "a.b.example.com"), 2);
    assert_int_equal(LOOK(r, "x.eu.example.com"), 3); /* sufijo más largo gana */
    assert_int_equal(LOOK(r, "www.example.com"), 4);  /* exacto antes que wildcard */
    assert_int_equal(LOOK(r, "example.com"), -1);     /* el wildcard no cubre el ápex */
    assert_int_equal(LOOK(r, "other.test"), -1);
    assert_int_equal(LOOK(r, ""), -1);

    assert_int_equal(router_add(r, "default", 9), 0);
    assert_int_equal(LOOK(r, "other.test"), 9);
    assert_int_equal(LOOK(r, "example.com"), 9);
    assert_int_equal(LOOK(r, ""), 9);
    router_free(r);
}

static void test_many_hosts(void **st)
{
    (void)st;
    router_t *r = router_new();
    char h[64];
    for (int i = 0; i < 2000; i++) {
        snprintf(h, sizeof(h), "host%d.test", i);
        assert_int_equal(router_add(r, h, i), 0);
    }
    router_build(r);
    for (int i = 0; i < 2000; i++) {
        snprintf(h, sizeof(h), "HOST%d.test", i);
        assert_int_equal(LOOK(r, h), i);
    }
    assert_int_equal(LOOK(r, "host2000.test"), -1);
    router_free(r);
}

static void test_tls_pattern(void **st)
{
    (void)st;
    assert_true(tls_pattern_match("api.test", "api.test", 8));
    assert_true(tls_pattern_match("api.test", "API.TEST", 8));
    assert_true(tls_pattern_match("*.example.com", "a.example.com", 13));
    assert_false(tls_pattern_match("*.example.com", "a.b.example.com", 15)); /* una etiqueta */
    assert_false(tls_pattern_match("*.example.com", "example.com", 11));
    assert_false(tls_pattern_match("api.test", "api.tes", 7));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_exact_wildcard_default),
        cmocka_unit_test(test_many_hosts),
        cmocka_unit_test(test_tls_pattern),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
