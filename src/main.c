#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "master.h"
#include "tls.h"
#include "util.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "uso: %s -c <config.toml> [-t] [-w N]\n"
            "  -c  fichero de configuración TOML\n"
            "  -t  solo valida la configuración y sale\n"
            "  -w  fuerza el número de workers\n"
            "Señales: SIGHUP recarga la configuración (también se recarga\n"
            "automáticamente al modificar el fichero); SIGTERM/SIGINT para.\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    bool test_only = false;
    int force_workers = 0;
    int opt;
    while ((opt = getopt(argc, argv, "c:tw:h")) != -1) {
        switch (opt) {
        case 'c':
            path = optarg;
            break;
        case 't':
            test_only = true;
            break;
        case 'w':
            force_workers = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (tls_global_init() < 0) {
        fprintf(stderr, "no se puede inicializar OpenSSL\n");
        return 1;
    }
    char *text = read_file(path, NULL);
    if (!text) {
        fprintf(stderr, "no se puede leer '%s': %s\n", path, strerror(errno));
        return 1;
    }
    char dir[CFG_PATH_MAX];
    str_copy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        str_copy(dir, ".", sizeof(dir));

    char err[512];
    config_t *cfg = config_parse(text, dir, err, sizeof(err));
    if (cfg && tls_validate_config(cfg, err, sizeof(err)) < 0) {
        config_free(cfg);
        cfg = NULL;
    }
    if (!cfg) {
        fprintf(stderr, "configuración inválida: %s\n", err);
        free(text);
        return 1;
    }
    if (test_only) {
        printf("configuración OK: %d frontends, %d backends, %d workers\n",
               cfg->nfrontends, cfg->nbackends, cfg->workers);
        config_free(cfg);
        free(text);
        return 0;
    }
    if (force_workers > 0 && force_workers <= 128)
        cfg->workers = force_workers;

    if (log_init(cfg->log_file, cfg->log_level, "master") < 0) {
        config_free(cfg);
        free(text);
        return 1;
    }
    int rc = master_run(path, cfg, text);
    log_shutdown();
    return rc;
}
