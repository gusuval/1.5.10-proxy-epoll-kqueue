#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_NAME_MAX 64
#define CFG_HOST_MAX 256
#define CFG_ADDR_MAX 128
#define CFG_PATH_MAX 512
#define CFG_MAX_SERVERS 64 /* por backend (máscara de reintentos de 64 bits) */

typedef enum {
    STRAT_ROUND_ROBIN = 0,
    STRAT_WEIGHTED,
    STRAT_LEAST_CONN,
    STRAT_LEAST_LOAD,
} strategy_t;

typedef enum { HEALTH_NONE = 0, HEALTH_TCP, HEALTH_HTTP } health_type_t;

typedef struct {
    char sni[CFG_HOST_MAX];
    char cert[CFG_PATH_MAX];
    char key[CFG_PATH_MAX];
} cfg_cert_t;

typedef struct {
    char host[CFG_HOST_MAX]; /* "api.test", "*.example.com" o "default" */
    char backend[CFG_NAME_MAX];
} cfg_route_t;

typedef struct {
    char name[CFG_NAME_MAX];
    char listen[CFG_ADDR_MAX];
    char key[CFG_ADDR_MAX]; /* dirección canónica "ip:puerto" */
    bool tls;
    cfg_cert_t *certs;
    int ncerts;
    int default_cert;
    cfg_route_t *routes;
    int nroutes;
} cfg_frontend_t;

typedef struct {
    char addr[CFG_ADDR_MAX];
    int weight;
} cfg_server_t;

typedef struct {
    health_type_t type;
    char path[CFG_HOST_MAX];
    int interval_ms;
    int timeout_ms;
    int rise;
    int fall;
} cfg_health_t;

typedef struct {
    char name[CFG_NAME_MAX];
    strategy_t strategy;
    int max_idle;
    cfg_server_t *servers;
    int nservers;
    cfg_health_t health;
} cfg_backend_t;

typedef struct {
    int client_header;
    int client_idle;
    int upstream_connect;
    int upstream_read;
    int upstream_idle;
} cfg_timeouts_t;

typedef struct config {
    int workers; /* ya resuelto ("auto" -> nº CPUs) */
    char stats_socket[CFG_PATH_MAX];
    char log_file[CFG_PATH_MAX];
    int log_level;
    bool access_log;
    cfg_timeouts_t timeouts;
    cfg_frontend_t *frontends;
    int nfrontends;
    cfg_backend_t *backends;
    int nbackends;
} config_t;

/*
 * Parsea y valida el texto TOML. base_dir se usa para resolver rutas
 * relativas (certificados, log). Devuelve NULL con el error en err.
 * No comprueba que los certificados sean cargables (lo hace tls.c).
 */
config_t *config_parse(const char *text, const char *base_dir, char *err,
                       size_t errlen);
config_t *config_load(const char *path, char *err, size_t errlen);
void config_free(config_t *c);

const char *strategy_name(strategy_t s);
int config_find_backend(const config_t *c, const char *name);

#endif
