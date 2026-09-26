#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CONTAINER_OF(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* Milisegundos de reloj monótono. */
uint64_t mono_ms(void);
/* Milisegundos de reloj de pared (epoch). */
uint64_t wall_ms(void);

int set_nonblock(int fd);
int set_cloexec(int fd);
void set_nodelay(int fd);

/* Copia con truncado, siempre termina en '\0'. Devuelve strlen(src). */
size_t str_copy(char *dst, const char *src, size_t dstsz);
/* Copia de n bytes (no necesariamente terminados) con truncado. */
void str_copy_n(char *dst, size_t dstsz, const char *src, size_t n);
bool str_ieq(const char *a, size_t alen, const char *b);
void str_lower(char *s);

/*
 * Parsea "host:port" o "[v6]:port". Si resolve es false solo acepta IP
 * numérica. Devuelve 0 o -1 con mensaje en err.
 */
int addr_parse(const char *s, bool resolve, struct sockaddr_storage *sa,
               socklen_t *salen, char *err, size_t errlen);
/* Forma canónica "ip:port" / "[ip]:port". */
void addr_format(const struct sockaddr *sa, char *out, size_t outlen);
/* IP sin puerto. */
void addr_ip(const struct sockaddr *sa, char *out, size_t outlen);

/* Lee un fichero completo en memoria (terminado en '\0'). */
char *read_file(const char *path, size_t *len);

/* Escribe todo el buffer en un fd bloqueante (reintenta EINTR). */
int write_all(int fd, const void *buf, size_t len);
/* Lee exactamente len bytes; espera con poll si el fd es no bloqueante. */
int read_full(int fd, void *buf, size_t len);

/* xorshift64* */
static inline uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

#endif
