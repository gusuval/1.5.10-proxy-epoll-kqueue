#ifndef WATCH_H
#define WATCH_H

/*
 * Vigilancia de un fichero de configuración.
 *   Linux: inotify sobre el directorio (cubre escritura in situ y guardado
 *          atómico por rename, como hacen vim o sed -i).
 *   kqueue: EVFILT_VNODE sobre el fichero y el directorio, en un kqueue
 *          propio cuyo fd se registra en el loop principal (un kqueue es
 *          pollable desde otro kqueue). Si el fichero se reemplaza se
 *          vuelve a abrir.
 * En ambos casos watch_fd() es legible cuando hay eventos.
 */

#include <stdbool.h>

typedef struct watch watch_t;

watch_t *watch_open(const char *path);
void watch_close(watch_t *w);
int watch_fd(const watch_t *w);
/* Drena los eventos; true si afectan al fichero vigilado. */
bool watch_consume(watch_t *w);

#endif
