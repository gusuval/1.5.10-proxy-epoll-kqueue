# Verificación de requerimientos

Cada requerimiento de [`requerimientos.md`](requerimientos.md) con la
evidencia que lo verifica.

- **UT**: `meson test -C build --suite unit` (cmocka, `tests/unit/`).
- **IT**: `tests/integration/run.sh` (96 comprobaciones). Entre comillas va el nombre del caso tal como lo imprime el script.
- **BM**: `bench/bench_proxy.sh`.

Estado: ✅ verificado automáticamente · 🔶 verificado parcialmente o por revisión · ⬜ no verificable en este entorno.

## Funcionales

| ID | Evidencia | Estado |
|---|---|---|
| RF-01 | Meson elige `io_event_epoll.c` / `io_event_kqueue.c` por plataforma (`src/meson.build`). Nada fuera de ellos incluye epoll/kqueue. UT `test_core` (readiness, timers). | ✅ Linux · 🔶 kqueue: solo comprobación de sintaxis |
| RF-02 | Revisión: todos los bucles de E/S (`connection.c`, `worker.c`, `test_backend.c`) iteran hasta `EAGAIN`. IT "POST 10 MB …" (cuerpos mucho mayores que un buffer) | ✅ |
| RF-03 | IT "worker muerto es relanzado", "tráfico normal tras relanzar el worker" | ✅ |
| RF-04 | Linux: `SO_REUSEPORT` por worker. IT "stats: agregado de 2 workers". macOS/BSD: el master abre los sockets y los pasa por `SCM_RIGHTS` | ✅ Linux · ⬜ macOS |
| RF-05 | UT `test_timers_order_cancel_lazy`, `test_now_fresh_in_handler` | ✅ |
| RF-10 | IT: frontends `web`, `web-tls`, `admin` y `extra` (este último añadido en caliente) | ✅ |
| RF-11 | IT "TLS verificado contra la CA", "TLS 1.3 negociado", "TLS 1.2 aceptado" | ✅ |
| RF-12 | IT "SNI api.test -> cert api.test", "SNI a.example.com -> cert wildcard", "SNI desconocido -> default_cert", "sin SNI -> default_cert" | ✅ |
| RF-13 | IT "SNI distinto de Host no cubierto -> 421" y "... cubierto por el cert -> 200" | ✅ |
| RF-14 | Revisión: `connect_upstream()` usa sockets TCP en claro | ✅ |
| RF-20 | UT `test_router`. IT "dominio exacto …", "Host con puerto y mayúsculas" | ✅ |
| RF-21 | UT `test_exact_wildcard_default`. IT "wildcard …", "ápex example.com -> default" | ✅ |
| RF-22 | IT "frontend sin default -> 404", "backend sin servidores sanos -> 503", "HTTP/1.1 sin Host -> 400" | ✅ |
| RF-23 | IT "cada petición keep-alive se enruta aparte" | ✅ |
| RF-30 | UT `test_fragmented_byte_by_byte`, `test_fragmented_random` | ✅ |
| RF-31 | UT `test_limits`, `test_malformed`. IT "cabecera > 16 KB -> 431", "petición malformada -> 400" | ✅ |
| RF-32 | IT "X-Forwarded-For concatenado", "X-Real-IP reemplazado", "X-Forwarded-Proto: http/https" | ✅ |
| RF-33 | UT `test_connection_tokens`. IT "hop-by-hop …", "cabecera listada en Connection eliminada" | ✅ |
| RF-34 | UT `test_smuggling`. IT "Content-Length + Transfer-Encoding -> 400" | ✅ |
| RF-35 | IT "POST 10 MB Content-Length íntegro" (SHA-256), "memoria acotada: pico RSS por worker < 64 MB" | ✅ |
| RF-36 | UT `test_body_chunked*`. IT "POST 10 MB chunked íntegro", "respuesta chunked", "chunked reenviado con trailers" | ✅ |
| RF-37 | IT "HEAD conserva Content-Length sin cuerpo", "204 sin cuerpo", "respuesta delimitada por cierre", "HEAD/204 + petición siguiente" | ✅ |
| RF-38 | IT "10 peticiones en 1 conexión cliente", "HTTP/1.0 …" | ✅ |
| RF-39 | IT "30 peticiones reutilizan conexiones al backend", "stats cuentan reutilizaciones". BM: ~800 conexiones al backend para ~27 M peticiones | ✅ |
| RF-40 | IT "reintento de conexión reutilizada que el backend descarta" (`test_backend --drop-every`), "conexiones del pool muertas no producen errores" | ✅ |
| RF-41 | IT "3 peticiones pipelined, 3 respuestas en orden", "pipelining con cuerpo intermedio" | ✅ |
| RF-42 | IT "upstream_read -> 504", "client_header incompleta -> 408". `upstream_connect`: por revisión (en loopback un connect rechazado falla al instante) | 🔶 |
| RF-43 | IT "WebSocket por HTTP", "WebSocket por TLS" (`tools/ws_probe`), "Upgrade no aceptado -> respuesta normal" | ✅ |
| RF-50..53 | UT `test_round_robin`, `test_weighted_smooth`, `test_least_conn`. IT "round_robin 30 -> 10/10/10", "weighted 3:1 -> 30/10", "least_conn evita al servidor lento" | ✅ |
| RF-54 | UT `test_least_load`. IT "least_load prefiere la carga baja" | ✅ |
| RF-55 | IT "X-Backend-Load eliminado hacia el cliente" | ✅ |
| RF-56 | UT `test_least_load_stale` | ✅ |
| RF-57 | UT `test_passive_health`. IT "backend caído: primer intento -> 502 … -> 503", "backend caído: 0 errores para el cliente" | ✅ |
| RF-58 | IT "health activo marca api-2 como down", "health activo recupera api-2 (rise)" | ✅ |
| RF-59 | UT `test_down_excluded` (las 4 estrategias) | ✅ |
| RF-60, 61 | UT `test_config` (17 reglas de validación, claves desconocidas) | ✅ |
| RF-62 | IT "recarga automática (sed -i, rename) < 1 s", "recarga automática (escritura in situ)" | ✅ Linux · ⬜ macOS |
| RF-63 | IT "recarga manual con SIGHUP" | ✅ |
| RF-64 | IT "config inválida rechazada y contada", "… el tráfico sigue", "… error en el log" | ✅ |
| RF-65 | IT "petición en vuelo termina con la config vieja" | ✅ |
| RF-66 | IT "frontend añadido/eliminado en caliente", "certificados recargados (default_cert)" | ✅ |
| RF-70 | UT `test_log_ring_basic`, `test_log_ring_concurrent` (4 productores × 20 000) | ✅ |
| RF-71 | IT "access log con campos" | ✅ |
| RF-72 | IT "stats: JSON válido", "stats: campos principales" | ✅ |
| RF-80..84 | Los usa IT en cada ejecución; `hosts.sh`: `add` dos veces no duplica y `remove` deja el fichero idéntico | ✅ |

## No funcionales

| ID | Evidencia | Estado |
|---|---|---|
| RNF-01, 02 | BM: resultados en el README y en `bench/results/` | ✅ Linux |
| RNF-03 | IT "wrk + 8 recargas: sin errores de socket / sin respuestas no-2xx" | ✅ |
| RNF-04 | Revisión: DNS solo al cargar la config; sondas en hilo aparte; log en hilo aparte. Excepción documentada: al recargar, cada worker lee los certificados del disco | 🔶 |
| RNF-05 | IT "memoria acotada …", "buffers liberados tras las subidas" | ✅ |
| RNF-06 | `meson setup build-asan -Db_sanitize=address,undefined` + UT + IT: 0 informes de ASan, UBSan y LeakSanitizer | ✅ |
| RNF-07 | RF-31 y RF-34 | ✅ |
| RNF-08 | Linux: compila y pasa todo. macOS/BSD: comprobación de sintaxis del código kqueue con cabeceras de imitación; sin ejecutar | 🔶 |
| RNF-09 | `warning_level=3` (`-Wall -Wextra -Wpedantic`): 0 warnings | ✅ |
| RNF-10 | `meson setup build && meson compile -C build` desde cero; tomlc99 vendorizado como subproject | ✅ |
