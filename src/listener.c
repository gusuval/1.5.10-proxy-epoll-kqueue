#include "listener.h"
#include "util.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int make_socket(const char *addr, bool reuseport, bool do_listen,
                       char *err, size_t errlen)
{
    struct sockaddr_storage sa;
    socklen_t salen;
    if (addr_parse(addr, false, &sa, &salen, err, errlen) < 0)
        return -1;
    int fd = socket(sa.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
        snprintf(err, errlen, "SO_REUSEPORT: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (sa.ss_family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, salen) < 0) {
        snprintf(err, errlen, "bind %s: %s", addr, strerror(errno));
        close(fd);
        return -1;
    }
    if (do_listen && listen(fd, SOMAXCONN > 4096 ? SOMAXCONN : 4096) < 0) {
        snprintf(err, errlen, "listen %s: %s", addr, strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    set_cloexec(fd);
    return fd;
}

int listener_open(const char *addr, bool reuseport, char *err, size_t errlen)
{
    return make_socket(addr, reuseport, true, err, errlen);
}

int listener_test_bind(const char *addr, char *err, size_t errlen)
{
    int fd = make_socket(addr, false, false, err, errlen);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

typedef struct {
    uint32_t magic, cmd, len, nfds;
} chan_hdr_t;

int chan_send(int fd, uint32_t cmd, const void *payload, uint32_t len,
              const int *fds, int nfds)
{
    chan_hdr_t h = {CHAN_MAGIC, cmd, len, (uint32_t)nfds};
    struct iovec iov = {.iov_base = &h, .iov_len = sizeof(h)};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    union {
        char buf[CMSG_SPACE(sizeof(int) * CHAN_MAX_FDS)];
        struct cmsghdr align;
    } cm;
    if (nfds > CHAN_MAX_FDS)
        return -1;
    if (nfds > 0) {
        memset(&cm, 0, sizeof(cm));
        msg.msg_control = cm.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    ssize_t n;
    do {
        n = sendmsg(fd, &msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -1;
    if ((size_t)n < sizeof(h) &&
        write_all(fd, (char *)&h + n, sizeof(h) - (size_t)n) < 0)
        return -1;
    return len ? write_all(fd, payload, len) : 0;
}

int chan_recv(int fd, chan_msg_t *m)
{
    chan_hdr_t h;
    struct iovec iov = {.iov_base = &h, .iov_len = sizeof(h)};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    union {
        char buf[CMSG_SPACE(sizeof(int) * CHAN_MAX_FDS)];
        struct cmsghdr align;
    } cm;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cm.buf;
    msg.msg_controllen = sizeof(cm.buf);
    ssize_t n;
    do {
        n = recvmsg(fd, &msg, MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : -1;
    if (n == 0)
        return -1;

    memset(m, 0, sizeof(*m));
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (k > CHAN_MAX_FDS)
                k = CHAN_MAX_FDS;
            memcpy(m->fds, CMSG_DATA(c), sizeof(int) * (size_t)k);
            m->nfds = k;
        }
    }
    if ((size_t)n < sizeof(h) &&
        read_full(fd, (char *)&h + n, sizeof(h) - (size_t)n) < 0)
        return -1;
    if (h.magic != CHAN_MAGIC || h.len > 64u * 1024 * 1024)
        return -1;
    m->cmd = h.cmd;
    m->len = h.len;
    m->payload = malloc((size_t)h.len + 1);
    if (!m->payload)
        return -1;
    if (h.len && read_full(fd, m->payload, h.len) < 0) {
        free(m->payload);
        return -1;
    }
    m->payload[h.len] = '\0';
    return 0;
}
