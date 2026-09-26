#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "http_parser.h"

static int parse_req(const char *s, http_msg_t *m)
{
    size_t scan = 0;
    return http_parse_request(m, s, strlen(s), 16384, &scan);
}

static void test_simple_get(void **st)
{
    (void)st;
    http_msg_t m;
    const char *r = "GET /index.html HTTP/1.1\r\nHost: Example.COM:8080\r\nAccept: */*\r\n\r\n";
    assert_int_equal(parse_req(r, &m), HTTP_COMPLETE);
    assert_int_equal(m.head_len, strlen(r));
    assert_int_equal(m.method_len, 3);
    assert_memory_equal(m.method, "GET", 3);
    assert_memory_equal(m.target, "/index.html", m.target_len);
    assert_int_equal(m.version_minor, 1);
    assert_int_equal(m.host_len, 11);
    assert_memory_equal(m.host, "Example.COM", 11);
    assert_int_equal(m.content_length, -1);
    assert_false(m.chunked);
    assert_int_equal(m.nheaders, 2);
}

/* Alimenta la petición byte a byte: solo debe completarse al final. */
static void test_fragmented_byte_by_byte(void **st)
{
    (void)st;
    const char *r = "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    size_t head = strlen(r) - 5;
    size_t scan = 0;
    http_msg_t m;
    for (size_t i = 1; i < head; i++)
        assert_int_equal(http_parse_request(&m, r, i, 16384, &scan), HTTP_INCOMPLETE);
    assert_int_equal(http_parse_request(&m, r, head, 16384, &scan), HTTP_COMPLETE);
    assert_int_equal(m.content_length, 5);
    assert_int_equal(m.head_len, head);
}

static void test_fragmented_random(void **st)
{
    (void)st;
    const char *r = "GET /a HTTP/1.1\r\nHost: h\r\nX-A: 1\r\nX-B: 2\r\n\r\n";
    size_t len = strlen(r);
    srand(42);
    for (int round = 0; round < 200; round++) {
        size_t scan = 0, have = 0;
        http_msg_t m;
        int rc = HTTP_INCOMPLETE;
        while (rc == HTTP_INCOMPLETE) {
            have += 1 + (size_t)(rand() % 7);
            if (have > len)
                have = len;
            rc = http_parse_request(&m, r, have, 16384, &scan);
            if (have < len)
                assert_int_equal(rc, HTTP_INCOMPLETE);
        }
        assert_int_equal(rc, HTTP_COMPLETE);
        assert_int_equal(m.nheaders, 3);
    }
}

static void test_limits(void **st)
{
    (void)st;
    char big[20000];
    strcpy(big, "GET / HTTP/1.1\r\nHost: a\r\nX-Big: ");
    size_t n = strlen(big);
    memset(big + n, 'a', 17000);
    big[n + 17000] = '\0';
    size_t scan = 0;
    http_msg_t m;
    assert_int_equal(http_parse_request(&m, big, strlen(big), 16384, &scan), HTTP_TOO_LARGE);
    strcat(big, "\r\n\r\n");
    scan = 0;
    assert_int_equal(http_parse_request(&m, big, strlen(big), 16384, &scan), HTTP_TOO_LARGE);
}

static void test_malformed(void **st)
{
    (void)st;
    http_msg_t m;
    assert_int_equal(parse_req("GARBAGE\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("GET / HTTP/2.0\r\nHost: a\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nHost : a\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nNoColon\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nHost: a\r\nX: a\nb\r\n\r\n", &m), HTTP_ERROR);
    /* HTTP/1.1 sin Host */
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nAccept: x\r\n\r\n", &m), HTTP_ERROR);
    /* HTTP/1.0 sin Host es válido */
    assert_int_equal(parse_req("GET / HTTP/1.0\r\n\r\n", &m), HTTP_COMPLETE);
    assert_false(m.has_host);
    /* Host duplicado */
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &m), HTTP_ERROR);
}

static void test_smuggling(void **st)
{
    (void)st;
    http_msg_t m;
    assert_int_equal(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
                               "Content-Length: 5\r\n\r\n", &m), HTTP_ERROR);
    assert_int_equal(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
                               "Content-Length: 4\r\n\r\n", &m), HTTP_COMPLETE);
    assert_int_equal(parse_req("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", &m),
                     HTTP_ERROR);
    assert_int_equal(parse_req("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n", &m),
                     HTTP_ERROR);
    assert_int_equal(parse_req("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", &m),
                     HTTP_COMPLETE);
    assert_true(m.chunked);
}

static void test_connection_tokens(void **st)
{
    (void)st;
    http_msg_t m;
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nHost: a\r\nConnection: keep-alive, X-Foo, Upgrade\r\n"
                               "Upgrade: websocket\r\nX-Foo: 1\r\nX-Bar: 2\r\n\r\n", &m), HTTP_COMPLETE);
    assert_true(m.conn_keepalive);
    assert_true(m.conn_upgrade);
    assert_false(m.conn_close);
    assert_memory_equal(m.upgrade, "websocket", 9);
    assert_true(http_is_hop_by_hop(&m, "X-Foo", 5));
    assert_true(http_is_hop_by_hop(&m, "keep-alive", 10));
    assert_true(http_is_hop_by_hop(&m, "TE", 2));
    assert_false(http_is_hop_by_hop(&m, "X-Bar", 5));
    /* nunca se elimina Content-Length aunque se liste en Connection */
    assert_int_equal(parse_req("GET / HTTP/1.1\r\nHost: a\r\nConnection: Content-Length\r\n\r\n", &m),
                     HTTP_COMPLETE);
    assert_false(http_is_hop_by_hop(&m, "Content-Length", 14));
}

static void test_leading_crlf(void **st)
{
    (void)st;
    http_msg_t m;
    assert_int_equal(parse_req("\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", &m), HTTP_COMPLETE);
    assert_memory_equal(m.method, "GET", 3);
}

static void test_response(void **st)
{
    (void)st;
    http_msg_t m;
    size_t scan = 0;
    const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nX-Backend-Load: 0.42\r\n\r\n";
    assert_int_equal(http_parse_response(&m, r, strlen(r), 16384, &scan), HTTP_COMPLETE);
    assert_int_equal(m.status, 200);
    assert_int_equal(m.content_length, 12);
    assert_memory_equal(m.backend_load, "0.42", 4);
    scan = 0;
    r = "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert_int_equal(http_parse_response(&m, r, strlen(r), 16384, &scan), HTTP_COMPLETE);
    assert_true(m.chunked);
    assert_int_equal(m.content_length, -1);
    scan = 0;
    r = "HTTP/1.1 204\r\n\r\n";
    assert_int_equal(http_parse_response(&m, r, strlen(r), 16384, &scan), HTTP_COMPLETE);
    assert_int_equal(m.status, 204);
    scan = 0;
    r = "HTTP/1.1 20 OK\r\n\r\n";
    assert_int_equal(http_parse_response(&m, r, strlen(r), 16384, &scan), HTTP_ERROR);
}

static void test_body_length(void **st)
{
    (void)st;
    http_body_t b;
    http_body_init(&b, BODY_LENGTH, 10);
    assert_int_equal(http_body_feed(&b, "12345", 5), 5);
    assert_false(b.done);
    assert_int_equal(http_body_feed(&b, "67890GET /", 10), 5);
    assert_true(b.done);
    http_body_init(&b, BODY_NONE, 0);
    assert_true(b.done);
}

static void test_body_chunked(void **st)
{
    (void)st;
    const char *c = "5\r\nhello\r\n1A;ext=v\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\nX-T: 1\r\n\r\nNEXT";
    size_t body = strlen(c) - 4;
    /* de una vez */
    http_body_t b;
    http_body_init(&b, BODY_CHUNKED, 0);
    assert_int_equal(http_body_feed(&b, c, strlen(c)), (ssize_t)body);
    assert_true(b.done);
    /* byte a byte */
    http_body_init(&b, BODY_CHUNKED, 0);
    size_t total = 0;
    for (size_t i = 0; i < strlen(c) && !b.done; i++)
        total += (size_t)http_body_feed(&b, c + i, 1);
    assert_true(b.done);
    assert_int_equal(total, body);
    /* sin trailers */
    http_body_init(&b, BODY_CHUNKED, 0);
    assert_int_equal(http_body_feed(&b, "0\r\n\r\n", 5), 5);
    assert_true(b.done);
}

static void test_body_chunked_invalid(void **st)
{
    (void)st;
    http_body_t b;
    http_body_init(&b, BODY_CHUNKED, 0);
    assert_int_equal(http_body_feed(&b, "zz\r\n", 4), -1);
    http_body_init(&b, BODY_CHUNKED, 0);
    assert_int_equal(http_body_feed(&b, "3\r\nabcX", 7), -1);
    http_body_init(&b, BODY_CHUNKED, 0);
    assert_int_equal(http_body_feed(&b, "ffffffffffffffff\r\n", 18), -1);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_simple_get),
        cmocka_unit_test(test_fragmented_byte_by_byte),
        cmocka_unit_test(test_fragmented_random),
        cmocka_unit_test(test_limits),
        cmocka_unit_test(test_malformed),
        cmocka_unit_test(test_smuggling),
        cmocka_unit_test(test_connection_tokens),
        cmocka_unit_test(test_leading_crlf),
        cmocka_unit_test(test_response),
        cmocka_unit_test(test_body_length),
        cmocka_unit_test(test_body_chunked),
        cmocka_unit_test(test_body_chunked_invalid),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
