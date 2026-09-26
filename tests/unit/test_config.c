#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "config.h"

#define BACKEND_OK "[[backend]]\nname = \"b\"\nservers = [ { addr = \"127.0.0.1:9001\" } ]\n"
#define FRONTEND_OK "[[frontend]]\nname = \"f\"\nlisten = \"127.0.0.1:8080\"\n" \
                    "  [[frontend.route]]\n  host = \"default\"\n  backend = \"b\"\n"

static char err[512];

static config_t *parse(const char *t) { return config_parse(t, "/etc/proxy", err, sizeof(err)); }

static void expect_error(const char *text, const char *needle)
{
    config_t *c = parse(text);
    if (c) {
        config_free(c);
        fail_msg("se esperaba error '%s'", needle);
    }
    if (!strstr(err, needle))
        fail_msg("error '%s' no contiene '%s'", err, needle);
}

static void test_full_example(void **st)
{
    (void)st;
    const char *t =
        "[global]\nworkers = 4\nstats_socket = \"/tmp/x.sock\"\nlog_file = \"p.log\"\n"
        "log_level = \"warn\"\naccess_log = false\n"
        "[timeouts]\nclient_idle = 1234\n"
        "[[frontend]]\nname = \"tls\"\nlisten = \"0.0.0.0:8443\"\ntls = true\n"
        "certs = [ { sni = \"*.Example.com\", cert = \"c.pem\", key = \"/abs/k.pem\" } ]\n"
        "  [[frontend.route]]\n  host = \"API.test\"\n  backend = \"api\"\n"
        "  [[frontend.route]]\n  host = \"default\"\n  backend = \"api\"\n"
        "[[backend]]\nname = \"api\"\nstrategy = \"least_load\"\nmax_idle = 8\n"
        "servers = [ { addr = \"127.0.0.1:9001\", weight = 3 }, { addr = \"[::1]:9002\" } ]\n"
        "  [backend.health]\n  type = \"http\"\n  path = \"/hc\"\n  interval = 1000\n";
    config_t *c = parse(t);
    assert_non_null(c);
    assert_int_equal(c->workers, 4);
    assert_string_equal(c->log_file, "/etc/proxy/p.log");
    assert_int_equal(c->log_level, 2);
    assert_false(c->access_log);
    assert_int_equal(c->timeouts.client_idle, 1234);
    assert_int_equal(c->timeouts.client_header, 10000); /* por defecto */
    assert_int_equal(c->nfrontends, 1);
    assert_true(c->frontends[0].tls);
    assert_string_equal(c->frontends[0].key, "0.0.0.0:8443");
    assert_string_equal(c->frontends[0].certs[0].sni, "*.example.com");
    assert_string_equal(c->frontends[0].certs[0].cert, "/etc/proxy/c.pem");
    assert_string_equal(c->frontends[0].certs[0].key, "/abs/k.pem");
    assert_string_equal(c->frontends[0].routes[0].host, "api.test");
    assert_int_equal(c->backends[0].strategy, STRAT_LEAST_LOAD);
    assert_int_equal(c->backends[0].max_idle, 8);
    assert_int_equal(c->backends[0].nservers, 2);
    assert_int_equal(c->backends[0].servers[0].weight, 3);
    assert_int_equal(c->backends[0].servers[1].weight, 1);
    assert_int_equal(c->backends[0].health.type, HEALTH_HTTP);
    assert_string_equal(c->backends[0].health.path, "/hc");
    assert_int_equal(c->backends[0].health.fall, 3);
    config_free(c);
}

static void test_minimal_defaults(void **st)
{
    (void)st;
    config_t *c = parse(FRONTEND_OK BACKEND_OK);
    assert_non_null(c);
    assert_true(c->workers >= 1);
    assert_int_equal(c->backends[0].strategy, STRAT_ROUND_ROBIN);
    assert_int_equal(c->backends[0].health.type, HEALTH_NONE);
    config_free(c);
}

static void test_validation_rules(void **st)
{
    (void)st;
    expect_error("x = = 1", "TOML inválido");
    expect_error(BACKEND_OK, "no hay ningún [[frontend]]");
    expect_error(FRONTEND_OK, "no hay ningún [[backend]]");
    expect_error(FRONTEND_OK BACKEND_OK BACKEND_OK, "backend 'b' duplicado");
    expect_error(FRONTEND_OK FRONTEND_OK BACKEND_OK, "frontend 'f' duplicado");
    expect_error(FRONTEND_OK "[[frontend]]\nname = \"g\"\nlisten = \"127.0.0.1:8080\"\n"
                 "  [[frontend.route]]\n  host = \"default\"\n  backend = \"b\"\n" BACKEND_OK,
                 "mismo puerto");
    expect_error("[[frontend]]\nname = \"f\"\nlisten = \"127.0.0.1:8080\"\n"
                 "  [[frontend.route]]\n  host = \"a\"\n  backend = \"nope\"\n" BACKEND_OK,
                 "backend inexistente 'nope'");
    expect_error("[[frontend]]\nname = \"f\"\nlisten = \"127.0.0.1:8080\"\n"
                 "  [[frontend.route]]\n  host = \"default\"\n  backend = \"b\"\n"
                 "  [[frontend.route]]\n  host = \"default\"\n  backend = \"b\"\n" BACKEND_OK,
                 "duplicado");
    expect_error("[[frontend]]\nname = \"f\"\nlisten = \"127.0.0.1:99999\"\n"
                 "  [[frontend.route]]\n  host = \"default\"\n  backend = \"b\"\n" BACKEND_OK,
                 "puerto inválido");
    expect_error("[[frontend]]\nname = \"f\"\nlisten = \"127.0.0.1:8443\"\ntls = true\n"
                 "  [[frontend.route]]\n  host = \"default\"\n  backend = \"b\"\n" BACKEND_OK,
                 "requiere al menos un certificado");
    expect_error(FRONTEND_OK "[[backend]]\nname = \"b\"\nservers = []\n", "al menos un servidor");
    expect_error(FRONTEND_OK "[[backend]]\nname = \"b\"\nstrategy = \"random\"\n"
                 "servers = [ { addr = \"127.0.0.1:1\" } ]\n", "strategy 'random'");
    expect_error(FRONTEND_OK "[[backend]]\nname = \"b\"\n"
                 "servers = [ { addr = \"127.0.0.1:1\", weight = 0 } ]\n", "fuera de rango");
    expect_error(FRONTEND_OK BACKEND_OK "[global]\nworkerz = 2\n", "clave desconocida 'workerz'");
    expect_error("[[frontend]]\nname = \"f\"\nlisten = \"127.0.0.1:8080\"\n"
                 "  [[frontend.route]]\n  host = \"bad host\"\n  backend = \"b\"\n" BACKEND_OK,
                 "host 'bad host' inválido");
    expect_error(FRONTEND_OK BACKEND_OK "[timeouts]\nclient_idle = 0\n", "fuera de rango");
    expect_error(FRONTEND_OK "[[backend]]\nname = \"b\"\n"
                 "servers = [ { addr = \"127.0.0.1\" } ]\n", "sin puerto");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_full_example),
        cmocka_unit_test(test_minimal_defaults),
        cmocka_unit_test(test_validation_rules),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
