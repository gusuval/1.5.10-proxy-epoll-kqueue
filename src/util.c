#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

void set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

size_t str_copy(char *dst, const char *src, size_t dstsz)
{
    size_t n = strlen(src);
    if (dstsz) {
        size_t c = n < dstsz - 1 ? n : dstsz - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

void str_copy_n(char *dst, size_t dstsz, const char *src, size_t n)
{
    if (!dstsz)
        return;
    if (n > dstsz - 1)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool str_ieq(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen)
        return false;
    for (size_t i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

void str_lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

int addr_parse(const char *s, bool resolve, struct sockaddr_storage *sa,
               socklen_t *salen, char *err, size_t errlen)
{
    char host[256];
    const char *port;

    if (s[0] == '[') {
        const char *end = strchr(s, ']');
        if (!end || end[1] != ':') {
            snprintf(err, errlen, "dirección inválida '%s'", s);
            return -1;
        }
        str_copy_n(host, sizeof(host), s + 1, (size_t)(end - s - 1));
        port = end + 2;
    } else {
        const char *colon = strrchr(s, ':');
        if (!colon) {
            snprintf(err, errlen, "dirección '%s' sin puerto", s);
            return -1;
        }
        str_copy_n(host, sizeof(host), s, (size_t)(colon - s));
        port = colon + 1;
    }
    char *endp;
    long p = strtol(port, &endp, 10);
    if (*port == '\0' || *endp != '\0' || p <= 0 || p > 65535) {
        snprintf(err, errlen, "puerto inválido en '%s'", s);
        return -1;
    }
    if (host[0] == '\0' || strcmp(host, "*") == 0)
        str_copy(host, "0.0.0.0", sizeof(host));

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | (resolve ? 0 : AI_NUMERICHOST);
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(err, errlen, "no se puede resolver '%s': %s", s,
                 gai_strerror(rc));
        return -1;
    }
    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *salen = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

void addr_ip(const struct sockaddr *sa, char *out, size_t outlen)
{
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)sa;
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], out, (socklen_t)outlen);
            return;
        }
        inet_ntop(AF_INET6, &s6->sin6_addr, out, (socklen_t)outlen);
    } else if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)sa;
        inet_ntop(AF_INET, &s4->sin_addr, out, (socklen_t)outlen);
    } else {
        str_copy(out, "unix", outlen);
    }
}

void addr_format(const struct sockaddr *sa, char *out, size_t outlen)
{
    char ip[INET6_ADDRSTRLEN];
    addr_ip(sa, ip, sizeof(ip));
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)sa;
        snprintf(out, outlen, "[%s]:%u", ip, ntohs(s6->sin6_port));
    } else if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)sa;
        snprintf(out, outlen, "%s:%u", ip, ntohs(s4->sin_port));
    } else {
        str_copy(out, ip, outlen);
    }
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        if (n + 1 >= cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        if (r == 0)
            break;
        n += r;
    }
    fclose(f);
    buf[n] = '\0';
    if (len)
        *len = n;
    return buf;
}

int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                poll(&pfd, 1, 1000);
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLIN};
                poll(&pfd, 1, 1000);
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
