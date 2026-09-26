#include "watch.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

struct watch {
    int fd;
    int wd;
    char name[NAME_MAX + 1];
};

watch_t *watch_open(const char *path)
{
    watch_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    char dir[PATH_MAX];
    str_copy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) {
        str_copy(w->name, slash + 1, sizeof(w->name));
        if (slash == dir)
            dir[1] = '\0';
        else
            *slash = '\0';
    } else {
        str_copy(w->name, path, sizeof(w->name));
        str_copy(dir, ".", sizeof(dir));
    }
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        free(w);
        return NULL;
    }
    w->wd = inotify_add_watch(w->fd, dir,
                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                  IN_DELETE | IN_MODIFY | IN_ATTRIB);
    if (w->wd < 0) {
        close(w->fd);
        free(w);
        return NULL;
    }
    return w;
}

void watch_close(watch_t *w)
{
    if (!w)
        return;
    close(w->fd);
    free(w);
}

int watch_fd(const watch_t *w) { return w->fd; }

bool watch_consume(watch_t *w)
{
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool hit = false;
    for (;;) {
        ssize_t n = read(w->fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)(void *)p;
            if (ev->len && strcmp(ev->name, w->name) == 0)
                hit = true;
            p += sizeof(*ev) + ev->len;
        }
    }
    return hit;
}
