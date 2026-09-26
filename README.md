# 🚄 Proxy Inverso de Alto Rendimiento — C11 + epoll/kqueue

## 🎯 Objetivo del proyecto

Implementar en **C11** un proxy inverso L7 asíncrono con terminación TLS, capaz de superar **50.000 peticiones/segundo sobre HTTPS** en Linux, portable entre Linux (`epoll`) y macOS (`kqueue`). Windows/IOCP queda fuera de alcance. La especificación detallada está en [`SPEC.md`](SPEC.md) y el catálogo de requerimientos en [`docs/requerimientos.md`](docs/requerimientos.md). Es el proyecto de sistemas del curso: aquí no hay framework — se programa directamente contra el sistema operativo.

Con este proyecto el alumno aprende:

- **I/O no bloqueante y event loops**: cómo un solo hilo atiende miles de conexiones (el modelo de nginx y Node.js por dentro).
- La diferencia entre `epoll` (Linux) y `kqueue` (macOS/BSD) y cómo abstraerlas tras una API común.
- Multiproceso con **`SO_REUSEPORT`** (un worker por CPU), balanceo round-robin/weighted/least-conn/least-load y health checks.
- Terminación **TLS** con OpenSSL y selección de certificado por **SNI**.
- Build con **Meson**, tests unitarios con **cmocka** y benchmarking con **wrk**.

## 🏗️ Arquitectura

```
Client ──► [Frontend :8080 HTTP / :8443 TLS+SNI] ──► parse HTTP Host header ──► route lookup
        ──► [Backend pool] ──► estrategia (rr/weighted/least_conn/least_load)
        ──► conexión upstream (pool keep-alive, HTTP en claro)
        ──► respuesta ──► keep-alive / pipelining / túnel WebSocket
```

- **Master + N workers** (`workers = "auto"` → nº de CPUs). El master valida la config, vigila el fichero, relanza workers caídos y sirve las estadísticas; cada worker tiene **un event-loop** no bloqueante y edge-triggered (`EPOLLET` / `EV_CLEAR`), más un hilo de health checks y otro de log.
- En Linux cada worker abre sus sockets con `SO_REUSEPORT` y el kernel reparte las conexiones. En macOS/BSD, donde `SO_REUSEPORT` no reparte carga, el master abre los sockets y los pasa a los workers (`SCM_RIGHTS`).
- Capa de abstracción `io_event.[ch]` con la misma API sobre epoll y kqueue.
- Configuración en **TOML** (frontends, rutas por dominio, backends con pesos) con **recarga en caliente automática** (vigilancia del fichero con inotify/kqueue) **y vía SIGHUP**.

### Módulos (C11)

| Módulo | Responsabilidad |
|--------|-----------------|
| `io_event` | Abstracción epoll/kqueue (`io_loop_create/add/mod/del/run/stop`), timers (min-heap perezoso) y callbacks diferidos |
| `master` | Arranque y relanzamiento de workers, recarga (fichero + SIGHUP), socket de estadísticas |
| `worker` | Event loop del worker, accept, swap de generaciones de config, parada ordenada |
| `listener` | Sockets de escucha (`SO_REUSEPORT`) y canal master→worker con paso de fds |
| `runtime` | Generación de config "compilada" (routers, TLS, backends) con refcount (RCU) |
| `tls` | Terminación TLS (OpenSSL), selección de certificado por SNI |
| `connection` | Máquina de estados cliente↔upstream, arena de conexiones |
| `http_parser` | Parser incremental HTTP/1.1 (Content-Length, chunked, Upgrade); extrae `Host:`; inyecta `X-Forwarded-For/Proto/Real-IP` |
| `router` | Hash djb2 para dominios exactos, wildcards `*.dom` (sufijo más largo), ruta default |
| `backend_pool` | round_robin / weighted / least_conn / least_load (`X-Backend-Load`) + pool keep-alive + health pasivo |
| `health` | Sondas TCP/HTTP activas en hilo aparte |
| `config` | Parser TOML (tomlc99 vendorizado), validación estricta (rechaza claves desconocidas) |
| `watch` | Vigilancia del fichero de config: inotify (Linux) / `EVFILT_VNODE` (kqueue) |
| `log` | Ring buffer 4096×512B con hilo consumidor |
| `buffer_pool` | Arena `mmap` de slots de 16 KB con freelist |
| `stats` | Memoria compartida entre procesos (contadores atómicos + seqlock) y JSON agregado por socket UNIX |

Herramientas de prueba (`tools/`): `test_backend` (backend HTTP/1.1 + WebSocket sobre la misma `io_event`) y `ws_probe` (cliente WebSocket en C, con TLS).

## ⚙️ Funcionalidades

- Múltiples frontends (puertos de escucha) y enrutamiento L7 por header `Host:`.
- Resolución de rutas: dominio exacto → wildcard `*.example.com` → `default` → `404` (sin backend sano → `503`).
- Frontends TLS con SNI (varios certificados por puerto).
- HTTP/1.1 completo: keep-alive cliente↔proxy y proxy↔backend, pipelining, cuerpos `Content-Length` y `chunked`, WebSocket (`Upgrade`).
- Balanceo `round_robin`, `weighted`, `least_conn` y `least_load` (según la cabecera `X-Backend-Load` que envían los backends) con exclusión de backends caídos.
- Health checks activos (sondas) y pasivos (fallos de conexión).
- **Recarga de configuración sin cortar conexiones**, automática al guardar el fichero o con SIGHUP (swap atómico del router).
- Endpoint de estadísticas JSON.

## 💡 Solución

1. **Edge-triggered obliga a drenar**: con `EPOLLET`/`EV_CLEAR` el kernel avisa una sola vez por cambio; cada handler lee/escribe hasta `EAGAIN`. Es más eficiente pero menos indulgente que level-triggered — el corazón didáctico del proyecto.
2. **Portabilidad por abstracción**: `io_event.h` define la API; `io_event_epoll.c` y `io_event_kqueue.c` la implementan. Meson detecta la plataforma y compila la correcta.
3. **Reload sin downtime**: al detectar un cambio en el fichero (inotify/kqueue) o recibir SIGHUP (self-pipe trick), se parsea la nueva config en memoria, se valida y se hace un **swap atómico del router** con refcount (RCU) — las conexiones en vuelo terminan con la config vieja.
4. **Logging sin bloquear el event loop**: los mensajes van a un ring buffer MPSC sin locks y un hilo aparte los escribe a disco; si se llena, se descarta y se cuenta.
5. **Cuatro buffers de 16 KB por conexión** (cliente→proxy, proxy→backend y vuelta): los cuerpos se reenvían en streaming, sin almacenarlos enteros. La contrapresión es natural: si un buffer se llena, se deja de leer del otro lado.
6. **Liberación diferida**: cerrar un fd y liberar su objeto mientras quedan eventos suyos en el mismo lote de `epoll_wait` sería un use-after-free. Los objetos se marcan como cerrados y se liberan al final de la iteración.

Las decisiones de diseño, con sus alternativas, están en [`docs/decisiones.md`](docs/decisiones.md). La trazabilidad requerimiento → test está en [`docs/verificacion.md`](docs/verificacion.md).

**Documentación completa:**
- [Documentación técnica (PDF)](docs/pdf/documentacion-tecnica.pdf): arquitectura, módulos, protocolos, pruebas y rendimiento.
- [Manual de usuario (PDF)](docs/pdf/manual-de-usuario.pdf): instalación, configuración, operación y resolución de problemas.
- [Presentación (PPTX)](docs/presentacion/proxy-l7.pptx): decisiones técnicas, benchmark y coste.
- [Resumen de la sesión](docs/Resumen.md).

Los fuentes de los PDF están en `docs/pdf/src/` (HTML + CSS de impresión, renderizados con Chromium headless) y la presentación se genera con `docs/presentacion/generar.mjs` (pptxgenjs).

## 📊 Benchmark (Linux, wrk, build release)

Meta: **≥ 50.000 req/s sobre HTTPS** (TLS 1.3, keep-alive), 30 s, 0 errores (`SPEC.md` §11). Resultado obtenido con `bench/bench_proxy.sh`:

| Configuración | Protocolo | Req/s | Latencia media | p99 | Errores |
|---|---|---|---|---|---|
| `-t4 -c100 -d30s` | HTTPS | **223.705** | 427.65us | 1.29ms | 0 |
| `-t4 -c200 -d30s` | HTTPS | **213.565** | 847.88us | 2.37ms | 0 |
| `-t4 -c400 -d30s` | HTTPS | **172.075** | 1.97ms | 4.78ms | 0 |
| `-t4 -c200 -d30s` | HTTP (informativo) | **337.904** | 525.66us | 1.80ms | 0 |

Meta **superada en los tres escenarios HTTPS** (entre 3,4× y 4,5×), con 0 errores y 0 respuestas no-2xx en 28,5 millones de peticiones. El pool de keep-alive hacia los backends abrió 816 conexiones para todas ellas.

| Entorno | |
|---|---|
| CPU | AMD Ryzen 7 7735HS (8 núcleos / 16 hilos), WSL2 |
| Kernel | Linux 6.18 (WSL2), OpenSSL 3.5.5 |
| Proxy | 8 workers, build release + LTO, access log desactivado |
| Backends | 2 × `test_backend` (2 hilos cada uno), respuesta de 100 B |
| Afinidad (`taskset`) | wrk: CPUs 0-3 · proxy: 4-11 · backends: 12-15 |

wrk, proxy y backends comparten máquina, así que las cifras son una cota inferior de lo que da el proxy solo. Informe completo: [`bench/results/bench-20260926-194903.md`](bench/results/bench-20260926-194903.md).

## 🚀 Cómo ejecutar

Requisitos: compilador C11, Meson ≥ 0.60, Ninja, OpenSSL ≥ 3.0 y cmocka. Para los tests y el benchmark: curl, jq, socat y wrk. En Ubuntu:

```bash
sudo apt-get install build-essential meson ninja-build pkg-config libssl-dev libcmocka-dev curl jq socat wrk
```

```bash
# Build
meson setup build && meson compile -C build

# Tests unitarios (5 suites cmocka)
meson test -C build --suite unit

# Tests de integración (96 comprobaciones: genera config y certificados,
# lanza 12 backends y el proxy, y limpia al terminar)
./tests/integration/run.sh

# Todo con AddressSanitizer + UBSan
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --suite unit && ./tests/integration/run.sh build-asan

# Demo con certificados de prueba
./tests/gen_config.sh demo
./build/tools/test_backend --port 19001 --name api-1 &
./build/src/proxy -c demo/proxy.toml
curl -H 'Host: api.test' http://127.0.0.1:18080/
curl --cacert demo/certs/ca.pem --resolve api.test:18443:127.0.0.1 https://api.test:18443/

# Opcional para la demo: dar de alta los dominios en /etc/hosts
sudo ./tests/hosts.sh add

# Validar una config sin arrancar / ejecutar
./build/src/proxy -t -c proxy.toml
./build/src/proxy -c proxy.toml

# Recargar configuración: basta con guardar proxy.toml, o bien
kill -HUP <pid-master>

# Estadísticas
nc -U /tmp/proxy.sock

# Benchmark (compila en release en build-release/, informe en bench/results/)
./bench/bench_proxy.sh
```

La referencia comentada de todas las opciones está en [`proxy.toml`](proxy.toml).

## ✅ Estado

- **Linux**: todo implementado y verificado. 5 suites unitarias, 96 comprobaciones de integración y el benchmark, también con ASan/UBSan sin informes.
- **macOS/BSD (kqueue)**: `io_event_kqueue.c` y `watch_kqueue.c` están implementados, pero en este entorno solo se ha comprobado su sintaxis. No se han compilado ni ejecutado en un Mac: hay que pasar los tests allí antes de darlos por buenos.
- **Windows/IOCP**: fuera de alcance.

<!-- BEGIN cc:que-se-valora -->
¡Hola! ¡Qué bueno que estés trabajando en tu proyecto "Proxy Epoll Kqueue"! Sé que es un reto, pero estoy aquí para ayudarte a entender qué es lo que buscamos cuando lo revisamos. Para que te quede claro, he preparado esta sección para el `README` de tu proyecto:

---

## 📋 Qué se valora

Cuando revisemos tu proyecto, nos fijaremos en varias cosas para entender qué tan bien lo has resuelto.

Primero, **lo que más pesa** es que tu proxy funcione como se espera y cumpla con todo lo que pide el enunciado. Queremos ver que hace lo que tiene que hacer, sin fallos y de forma robusta.

También le damos un **peso importante** a la calidad de tu código y a la arquitectura que has elegido. Nos interesa que tu código sea claro, fácil de entender y que la estructura general de tu proyecto tenga sentido y esté bien pensada.

El **vídeo demo** también tiene un **peso importante**. Es tu oportunidad para mostrarnos cómo funciona tu proxy en acción y explicarnos de forma concisa lo que has hecho.

Finalmente, aunque con un **peso menor**, valoramos la documentación que incluyas y las decisiones que hayas tomado. Nos ayuda a entender tu proceso de pensamiento y por qué hiciste las cosas de cierta manera.

Recuerda que el detalle del enunciado es lo que manda para saber qué se espera de tu proyecto, y la evaluación no penaliza por lo que el enunciado no pide explícitamente.

---
<!-- END cc:que-se-valora -->
