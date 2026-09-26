# Resumen de la sesión

Sesión del 26-09-2026 (hora local, UTC−3) con Claude Code (Claude Opus 5.5). Punto de partida: un `README.md` y un `PROMPT.md` con requisitos breves. Resultado: un proxy inverso L7 en C11 implementado, probado, medido y enviado a revisión.

## Línea de tiempo

| Hora | Paso | Resultado |
|---|---|---|
| 18:32 | Refinar las specs de `PROMPT.md` y `README.md`, preguntando las dudas | 12 preguntas en 3 rondas. `SPEC.md` nuevo y README coherente |
| ~18:45 | Redactar los requerimientos en `docs/requerimientos.md` | RF-01…RF-84 y RNF-01…RNF-10, con criterios de aceptación y trazabilidad |
| ~19:10 | Implementar | Entorno sin compilador: el usuario instaló las dependencias. Código, tests y herramientas |
| ~19:40 | Pruebas y corrección de bugs | 93 → 96 comprobaciones de integración en verde, también con ASan/UBSan |
| ~19:45 | Benchmark | 172k–224k req/s HTTPS, 0 errores |
| 19:59 | Commit en la rama `feat/proxy-l7` | 4 commits temáticos |
| ~20:10 | Push y MR | Credenciales caducadas; tras autenticarse el usuario, [MR !1](https://gitlab.codecrypto.academy/uval.gustavo/1.5.10-proxy-epoll-kqueue/-/merge_requests/1) |
| ~20:15 | Documentación en PDF, presentación y este resumen | Técnica (17 págs.), manual (16 págs.), PPTX de 10 diapositivas |

## Decisiones tomadas con el usuario

| Tema | Decisión |
|---|---|
| Windows / IOCP | Fuera de alcance |
| Balanceo por carga | El backend envía `X-Backend-Load` en cada respuesta y el proxy la elimina antes de reenviarla al cliente |
| Recarga | Automática (vigilando el fichero) **y** por SIGHUP |
| TLS | Obligatorio, con SNI; hacia los backends, HTTP en claro |
| Keep-alive | En ambos lados (cliente y pool de conexiones a los backends) |
| HTTP | Content-Length, chunked, pipelining y WebSocket |
| Backend de pruebas | Sobre la misma abstracción `io_event` (kqueue y epoll) |
| Dominios en los tests | `curl --resolve`; script opcional para `/etc/hosts` |
| Benchmark | ≥ 50k req/s **sobre HTTPS**, solo en Linux |
| Tests de WebSocket | Sin python3-websockets (petición del usuario): cliente propio en C (`ws_probe`) |

Otras decisiones las tomó Claude y están justificadas en [`decisiones.md`](decisiones.md): errores locales con `Connection: close`, health por worker, reintento solo para peticiones idempotentes, pipelining en serie, etc.

## Qué se construyó

| Parte | Contenido | Líneas |
|---|---|---|
| `src/` | Proxy: 38 ficheros, 15 módulos (event loop, master/worker, HTTP, TLS, router, balanceo, health, config, recarga, log, estadísticas) | ~6.800 |
| `tools/` | `test_backend` (HTTP/1.1 + WebSocket) y `ws_probe` (cliente WebSocket, con TLS) | ~830 |
| `tests/` | 31 tests cmocka en 5 suites, script de integración con 96 comprobaciones, `gen_config.sh`, `hosts.sh` | ~1.500 |
| `bench/` | `bench_proxy.sh` (release + LTO, afinidad de CPU, informe) | ~170 |
| Documentación | `SPEC.md`, `requerimientos.md`, `decisiones.md`, `verificacion.md`, README, `proxy.toml` comentado | ~900 |

La dependencia tomlc99 (MIT) está vendorizada como subproject de Meson.

## Resultados

- **Tests**: 5/5 suites unitarias y 96/96 comprobaciones de integración, estables en ejecuciones repetidas. Con AddressSanitizer + UBSan + LeakSanitizer, 0 informes. 0 warnings con `-Wall -Wextra -Wpedantic`.
- **Benchmark** (WSL2, Ryzen 7 7735HS, 8 workers, 30 s por escenario):

  | Escenario | Req/s | p99 |
  |---|---|---|
  | HTTPS, 100 conexiones | 223.705 | 1,29 ms |
  | HTTPS, 200 conexiones | 213.565 | 2,37 ms |
  | HTTPS, 400 conexiones | 172.075 | 4,78 ms |
  | HTTP, 200 conexiones | 337.904 | 1,80 ms |

  28,5 M peticiones sin errores; la meta de 50k se supera entre 3,4× y 4,5×.

## Bugs encontrados por los tests

1. **Reloj del event loop atrasado** (real, grave): `io_loop_now()` se actualizaba después de despachar los eventos. Tras un periodo ocioso, los deadlines nacían vencidos y habría habido 408/504 espurios. Se corrigió en epoll y kqueue y tiene test de regresión.
2. **Desempate aleatorio en `least_load`**: un test salía inestable (35/40). Ahora se prefiere el servidor sin carga conocida, para conocerla cuanto antes.
3. **Dos bugs en `test_backend`**: al rebobinar el buffer de entrada se corrompían los cuerpos grandes.
4. **Tres errores del propio script de tests**: salto de línea eliminado por `$(...)`, recuento de peticiones propias a `/stats`, y `pgrep` que se encontraba a sí mismo.

## Incidencias del entorno

- No había compilador ni Meson; el usuario instaló los paquetes (sin sudo no se podía).
- El push y el MR fallaron por falta de credenciales (token de `glab` caducado, claves SSH no dadas de alta). Se resolvió con `glab auth login`.
- Para generar los PDF no había pandoc ni LaTeX. Se usó Chromium headless de Playwright, con sus librerías de sistema extraídas de paquetes `.deb` sin instalarlos.

## Pendiente

- **Probar en macOS/BSD**: el código kqueue solo tiene comprobada la sintaxis.
- Revisar y fusionar el MR !1.
- Vídeo demo (lo pide la rúbrica).
- Coste de la sesión: lo aporta el usuario (`/cost`) para la presentación.
