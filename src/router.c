#include "router.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *host;
    size_t len;
    unsigned long hash;
    int target;
} exact_t;

typedef struct {
    char *suffix; /* ".example.com" */
    size_t len;
    int target;
} wild_t;

struct router {
    exact_t *tab; /* tamaño potencia de 2 */
    size_t cap, n;
    wild_t *wild;
    size_t nwild, capwild;
    int def;
};

unsigned long djb2(const char *s, size_t len)
{
    unsigned long h = 5381;
    for (size_t i = 0; i < len; i++)
        h = ((h << 5) + h) + (unsigned char)tolower((unsigned char)s[i]);
    return h;
}

router_t *router_new(void)
{
    router_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->cap = 16;
    r->tab = calloc(r->cap, sizeof(exact_t));
    if (!r->tab) {
        free(r);
        return NULL;
    }
    r->def = -1;
    return r;
}

void router_free(router_t *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->cap; i++)
        free(r->tab[i].host);
    for (size_t i = 0; i < r->nwild; i++)
        free(r->wild[i].suffix);
    free(r->tab);
    free(r->wild);
    free(r);
}

static void insert(exact_t *tab, size_t cap, exact_t e)
{
    size_t i = e.hash & (cap - 1);
    while (tab[i].host)
        i = (i + 1) & (cap - 1);
    tab[i] = e;
}

static int grow(router_t *r)
{
    size_t nc = r->cap * 2;
    exact_t *nt = calloc(nc, sizeof(exact_t));
    if (!nt)
        return -1;
    for (size_t i = 0; i < r->cap; i++)
        if (r->tab[i].host)
            insert(nt, nc, r->tab[i]);
    free(r->tab);
    r->tab = nt;
    r->cap = nc;
    return 0;
}

static char *lower_dup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d)
        return NULL;
    for (size_t i = 0; i < n; i++)
        d[i] = (char)tolower((unsigned char)s[i]);
    d[n] = '\0';
    return d;
}

int router_add(router_t *r, const char *pattern, int target)
{
    if (strcmp(pattern, "default") == 0) {
        r->def = target;
        return 0;
    }
    if (pattern[0] == '*' && pattern[1] == '.') {
        if (r->nwild == r->capwild) {
            size_t nc = r->capwild ? r->capwild * 2 : 8;
            wild_t *nw = realloc(r->wild, nc * sizeof(wild_t));
            if (!nw)
                return -1;
            r->wild = nw;
            r->capwild = nc;
        }
        size_t len = strlen(pattern + 1);
        char *suf = lower_dup(pattern + 1, len);
        if (!suf)
            return -1;
        r->wild[r->nwild++] = (wild_t){.suffix = suf, .len = len, .target = target};
        return 0;
    }
    if ((r->n + 1) * 2 > r->cap && grow(r) < 0)
        return -1;
    size_t len = strlen(pattern);
    char *h = lower_dup(pattern, len);
    if (!h)
        return -1;
    insert(r->tab, r->cap,
           (exact_t){.host = h, .len = len, .hash = djb2(h, len), .target = target});
    r->n++;
    return 0;
}

static int wild_cmp(const void *a, const void *b)
{
    const wild_t *x = a, *y = b;
    return (x->len < y->len) - (x->len > y->len); /* descendente */
}

void router_build(router_t *r)
{
    if (r->nwild > 1)
        qsort(r->wild, r->nwild, sizeof(wild_t), wild_cmp);
}

static bool ieq_n(const char *a, const char *lower, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != lower[i])
            return false;
    return true;
}

int router_lookup(const router_t *r, const char *host, size_t len)
{
    if (len) {
        unsigned long h = djb2(host, len);
        for (size_t i = h & (r->cap - 1); r->tab[i].host; i = (i + 1) & (r->cap - 1)) {
            const exact_t *e = &r->tab[i];
            if (e->hash == h && e->len == len && ieq_n(host, e->host, len))
                return e->target;
        }
        /* Wildcards ordenados por sufijo más largo: el primero que casa gana.
         * "*.example.com" casa "a.example.com" (y "a.b.example.com"), no
         * "example.com". */
        for (size_t i = 0; i < r->nwild; i++) {
            const wild_t *w = &r->wild[i];
            if (len > w->len && ieq_n(host + len - w->len, w->suffix, w->len))
                return w->target;
        }
    }
    return r->def;
}
