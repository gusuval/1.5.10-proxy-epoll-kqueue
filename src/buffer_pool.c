#include "buffer_pool.h"

#include <stdlib.h>
#include <sys/mman.h>

typedef struct chunk {
    struct chunk *next;
    void *mem;
    size_t size;
} chunk_t;

struct bufpool {
    size_t per_chunk;
    void *free_head;
    chunk_t *chunks;
    size_t allocated;
    size_t in_use;
};

bufpool_t *bufpool_create(size_t slots_per_chunk)
{
    bufpool_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->per_chunk = slots_per_chunk ? slots_per_chunk : 256;
    return p;
}

void bufpool_destroy(bufpool_t *p)
{
    if (!p)
        return;
    chunk_t *c = p->chunks;
    while (c) {
        chunk_t *n = c->next;
        munmap(c->mem, c->size);
        free(c);
        c = n;
    }
    free(p);
}

static bool grow(bufpool_t *p)
{
    size_t size = p->per_chunk * BUF_SIZE;
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED)
        return false;
    chunk_t *c = malloc(sizeof(*c));
    if (!c) {
        munmap(mem, size);
        return false;
    }
    c->mem = mem;
    c->size = size;
    c->next = p->chunks;
    p->chunks = c;
    /* Encadenar en orden inverso para que los slots bajos salgan primero. */
    for (size_t i = p->per_chunk; i-- > 0;) {
        char *slot = (char *)mem + i * BUF_SIZE;
        *(void **)(void *)slot = p->free_head;
        p->free_head = slot;
    }
    p->allocated += p->per_chunk;
    return true;
}

char *bufpool_get(bufpool_t *p)
{
    if (!p->free_head && !grow(p))
        return NULL;
    char *slot = p->free_head;
    p->free_head = *(void **)(void *)slot;
    p->in_use++;
    return slot;
}

void bufpool_put(bufpool_t *p, char *slot)
{
    *(void **)(void *)slot = p->free_head;
    p->free_head = slot;
    p->in_use--;
}

size_t bufpool_in_use(const bufpool_t *p) { return p->in_use; }
size_t bufpool_allocated(const bufpool_t *p) { return p->allocated; }
