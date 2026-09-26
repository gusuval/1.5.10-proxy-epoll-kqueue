#ifndef MASTER_H
#define MASTER_H

#include "config.h"

/* Arranca workers y atiende señales/recargas hasta SIGTERM/SIGINT. */
int master_run(const char *cfg_path, config_t *cfg, char *cfg_text);

#endif
