#include "watch.h"
#include "util.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

struct watch {
    int kq;
    int ffd; /* fichero */
    int dfd; /* directorio */
    char path[PATH_MAX];
};

static void arm_file(watch_t *w)
{
    if (w->ffd >= 0)
        close(w->ffd);
    w->ffd = open(w->path, O_EVTONLY | O_CLOEXEC);
    if (w->ffd < 0)
        return;
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)w->ffd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME, 0, NULL);
    kevent(w->kq, &ev, 1, NULL, 0, NULL);
}

watch_t *watch_open(const char *path)
{
    watch_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    str_copy(w->path, path, sizeof(w->path));
    w->ffd = w->dfd = -1;
    w->kq = kqueue();
    if (w->kq < 0) {
        free(w);
        return NULL;
    }
    char dir[PATH_MAX];
    str_copy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = slash == dir ? (dir[1] = '\0', '/') : '\0';
    else
        str_copy(dir, ".", sizeof(dir));
    w->dfd = open(dir, O_EVTONLY | O_CLOEXEC);
    if (w->dfd >= 0) {
        struct kevent ev;
        EV_SET(&ev, (uintptr_t)w->dfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0, NULL);
        kevent(w->kq, &ev, 1, NULL, 0, NULL);
    }
    arm_file(w);
    return w;
}

void watch_close(watch_t *w)
{
    if (!w)
        return;
    if (w->ffd >= 0)
        close(w->ffd);
    if (w->dfd >= 0)
        close(w->dfd);
    close(w->kq);
    free(w);
}

int watch_fd(const watch_t *w) { return w->kq; }

bool watch_consume(watch_t *w)
{
    struct kevent evs[16];
    struct timespec zero = {0, 0};
    bool hit = false, rearm = false;
    int n;
    while ((n = kevent(w->kq, NULL, 0, evs, 16, &zero)) > 0) {
        for (int i = 0; i < n; i++) {
            if ((int)evs[i].ident == w->ffd) {
                hit = true;
                if (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME))
                    rearm = true;
            } else if ((int)evs[i].ident == w->dfd) {
                /* Cambio en el directorio: puede ser el reemplazo atómico
                 * del fichero (rename sobre él). */
                hit = true;
                rearm = true;
            }
        }
    }
    if (rearm || w->ffd < 0)
        arm_file(w);
    return hit;
}
