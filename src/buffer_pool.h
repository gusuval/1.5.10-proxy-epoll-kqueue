#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

/*
 * Arena de slots de BUF_SIZE bytes obtenida con mmap, con freelist
 * intrusiva. No es thread-safe: hay un pool por hilo de event loop.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BUF_SIZE 16384u

typedef struct bufpool bufpool_t;

bufpool_t *bufpool_create(size_t slots_per_chunk);
void bufpool_destroy(bufpool_t *p);
char *bufpool_get(bufpool_t *p);
void bufpool_put(bufpool_t *p, char *slot);
size_t bufpool_in_use(const bufpool_t *p);
size_t bufpool_allocated(const bufpool_t *p);

/* Buffer lineal sobre un slot: datos válidos en [r, w). */
typedef struct buf {
    char *data;
    uint32_t r, w;
} buf_t;

static inline size_t buf_len(const buf_t *b) { return b->w - b->r; }
static inline size_t buf_space(const buf_t *b) { return b->data ? BUF_SIZE - b->w : 0; }
static inline char *buf_rptr(const buf_t *b) { return b->data + b->r; }
static inline char *buf_wptr(const buf_t *b) { return b->data + b->w; }

static inline void buf_consume(buf_t *b, size_t n)
{
    b->r += (uint32_t)n;
    if (b->r == b->w)
        b->r = b->w = 0;
}

/* Mueve los datos al inicio para liberar espacio al final. */
static inline void buf_compact(buf_t *b)
{
    if (b->r == 0)
        return;
    size_t n = buf_len(b);
    if (n)
        memmove(b->data, b->data + b->r, n);
    b->r = 0;
    b->w = (uint32_t)n;
}

static inline bool buf_ensure(bufpool_t *p, buf_t *b)
{
    if (!b->data) {
        b->data = bufpool_get(p);
        b->r = b->w = 0;
    }
    return b->data != NULL;
}

static inline void buf_release(bufpool_t *p, buf_t *b)
{
    if (b->data) {
        bufpool_put(p, b->data);
        b->data = NULL;
    }
    b->r = b->w = 0;
}

/* Copia de src a dst todo lo que quepa. Devuelve bytes movidos. */
static inline size_t buf_move(buf_t *dst, buf_t *src)
{
    if (!buf_space(dst))
        buf_compact(dst);
    size_t n = buf_len(src);
    size_t sp = buf_space(dst);
    if (n > sp)
        n = sp;
    if (n) {
        memcpy(buf_wptr(dst), buf_rptr(src), n);
        dst->w += (uint32_t)n;
        buf_consume(src, n);
    }
    return n;
}

#endif
