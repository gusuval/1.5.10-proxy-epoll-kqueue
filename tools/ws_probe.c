/*
 * Cliente WebSocket mínimo para los tests de integración.
 *
 *   ws_probe [--tls] [--ca FICHERO] [--sni NOMBRE] [--host HOST]
 *            IP PUERTO RUTA MENSAJE
 *
 * Hace el handshake, envía MENSAJE como frame de texto enmascarado,
 * comprueba que el eco es idéntico, envía un close y sale con 0.
 */
#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

static SSL *ssl;
static int fd = -1;

static int io_write(const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        int r = ssl ? SSL_write(ssl, c, (int)n) : (int)send(fd, c, n, 0);
        if (r <= 0)
            return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

static int io_read(void *p, size_t n)
{
    return ssl ? SSL_read(ssl, p, (int)n) : (int)recv(fd, p, n, 0);
}

static int read_exact(unsigned char *p, size_t n, unsigned char *pre, size_t *prelen)
{
    size_t got = 0;
    size_t take = *prelen < n ? *prelen : n;
    memcpy(p, pre, take);
    memmove(pre, pre + take, *prelen - take);
    *prelen -= take;
    got = take;
    while (got < n) {
        int r = io_read(p + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static void die(const char *m)
{
    fprintf(stderr, "ws_probe: %s\n", m);
    exit(1);
}

int main(int argc, char **argv)
{
    bool tls = false;
    const char *ca = NULL, *sni = NULL, *host = NULL;
    static const struct option lo[] = {{"tls", no_argument, NULL, 't'},
                                       {"ca", required_argument, NULL, 'c'},
                                       {"sni", required_argument, NULL, 's'},
                                       {"host", required_argument, NULL, 'H'},
                                       {NULL, 0, NULL, 0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "tc:s:H:", lo, NULL)) != -1) {
        switch (ch) {
        case 't': tls = true; break;
        case 'c': ca = optarg; break;
        case 's': sni = optarg; break;
        case 'H': host = optarg; break;
        default: return 2;
        }
    }
    if (argc - optind != 4) {
        fprintf(stderr, "uso: ws_probe [--tls] [--ca F] [--sni N] [--host H] IP PUERTO RUTA MENSAJE\n");
        return 2;
    }
    const char *ip = argv[optind], *port = argv[optind + 1], *path = argv[optind + 2],
               *msg = argv[optind + 3];
    if (!host)
        host = sni ? sni : ip;

    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
    if (getaddrinfo(ip, port, &hints, &res) != 0)
        die("getaddrinfo");
    fd = socket(res->ai_family, SOCK_STREAM, 0);
    struct timeval tv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
        die("connect");
    freeaddrinfo(res);

    if (tls) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (ca) {
            SSL_CTX_load_verify_locations(ctx, ca, NULL);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        }
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (sni) {
            SSL_set_tlsext_host_name(ssl, sni);
            SSL_set1_host(ssl, sni);
        }
        if (SSL_connect(ssl) != 1) {
            ERR_print_errors_fp(stderr);
            die("handshake TLS");
        }
    }

    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     path, host);
    if (io_write(req, (size_t)n) < 0)
        die("envío del handshake");

    unsigned char buf[65536];
    size_t len = 0;
    char *end = NULL;
    while (!end) {
        int r = io_read(buf + len, sizeof(buf) - len - 1);
        if (r <= 0)
            die("respuesta del handshake");
        len += (size_t)r;
        buf[len] = 0;
        end = strstr((char *)buf, "\r\n\r\n");
    }
    if (strncmp((char *)buf, "HTTP/1.1 101", 12) != 0) {
        fprintf(stderr, "ws_probe: esperaba 101, recibido:\n%s\n", buf);
        return 1;
    }
    if (!strstr((char *)buf, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))
        die("Sec-WebSocket-Accept incorrecto");
    size_t hl = (size_t)(end + 4 - (char *)buf);
    memmove(buf, buf + hl, len - hl);
    len -= hl;

    size_t ml = strlen(msg);
    if (ml > 65535)
        die("mensaje demasiado largo");
    unsigned char frame[65536 + 8];
    size_t fl = 0;
    frame[fl++] = 0x81;
    if (ml < 126) {
        frame[fl++] = (unsigned char)(0x80 | ml);
    } else {
        frame[fl++] = 0x80 | 126;
        frame[fl++] = (unsigned char)(ml >> 8);
        frame[fl++] = (unsigned char)ml;
    }
    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + fl, mask, 4);
    fl += 4;
    for (size_t i = 0; i < ml; i++)
        frame[fl++] = (unsigned char)msg[i] ^ mask[i & 3];
    if (io_write(frame, fl) < 0)
        die("envío del frame");

    unsigned char h[4];
    if (read_exact(h, 2, buf, &len) < 0)
        die("lectura del eco");
    size_t pl = h[1] & 0x7f;
    if (pl == 126) {
        if (read_exact(h + 2, 2, buf, &len) < 0)
            die("lectura del eco");
        pl = ((size_t)h[2] << 8) | h[3];
    }
    unsigned char *payload = malloc(pl + 1);
    if (read_exact(payload, pl, buf, &len) < 0)
        die("lectura del payload");
    payload[pl] = 0;
    if ((h[0] & 0x0f) != 1 || pl != ml || memcmp(payload, msg, ml) != 0) {
        fprintf(stderr, "ws_probe: eco distinto: '%s'\n", payload);
        return 1;
    }
    unsigned char closef[6] = {0x88, 0x80, 0, 0, 0, 0};
    io_write(closef, sizeof(closef));
    printf("ws ok: %s\n", payload);
    free(payload);
    return 0;
}
