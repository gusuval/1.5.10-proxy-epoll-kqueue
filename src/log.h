#ifndef LOG_H
#define LOG_H

/*
 * Logging asíncrono: los productores (event loop, hilo de health) copian
 * el mensaje a un ring buffer MPSC de LOG_RING_SLOTS x LOG_SLOT_SIZE y un
 * hilo consumidor lo escribe al fichero. Nunca se bloquea al productor: si
 * el ring está lleno el mensaje se descarta y se cuenta.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_RING_SLOTS 4096u
#define LOG_SLOT_SIZE 512u

enum { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_ACCESS };

/* path NULL, "" o "-" = stderr. tag identifica el proceso ("master", "w3"). */
int log_init(const char *path, int level, const char *tag);
/* Cambia el nivel en caliente (p. ej. tras una recarga). */
void log_set_level(int level);
int log_level_from_str(const char *s);
void log_shutdown(void);
/* Tras fork(): descarta el estado heredado y arranca un consumidor nuevo. */
int log_reinit_child(const char *tag);

void log_msg(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void log_vmsg(int level, const char *fmt, va_list ap);
uint64_t log_dropped(void);

#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)

/* Ring MPSC expuesto para tests unitarios. */
typedef struct log_ring log_ring_t;
log_ring_t *log_ring_new(size_t slots);
void log_ring_free(log_ring_t *r);
bool log_ring_push(log_ring_t *r, const char *msg, size_t len);
/* Devuelve longitud copiada o -1 si vacío. */
int log_ring_pop(log_ring_t *r, char *out, size_t outlen);

#endif
