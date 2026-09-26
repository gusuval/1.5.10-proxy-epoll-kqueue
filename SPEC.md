# Especificación — Proxy inverso L7 en C11 (epoll/kqueue)

Este documento concreta los requisitos de `PROMPT.md` y resuelve las
ambigüedades con el `README.md`. Cuando un punto de aquí contradice a otro
documento, manda este.

Palabras clave: **DEBE** = obligatorio, **DEBERÍA** = recomendado,
**PUEDE** = opcional.

---

## 1. Alcance

| Incluido | Fuera de alcance |
|---|---|
| Linux (`epoll`) y macOS/BSD (`kqueue`) | Windows / IOCP |
| HTTP/1.0 y HTTP/1.1 (cliente↔proxy y proxy↔backend) | HTTP/2, HTTP/3 |
| Terminación TLS en el proxy (OpenSSL ≥ 3.0) con SNI | TLS hacia los backends (van en claro) |
| Keep-alive en ambos lados, pipelining, chunked, WebSocket | Caché de respuestas, compresión |
| Recarga de configuración automática y por `SIGHUP` | API de administración remota |

## 2. Plataforma y build

- **C11** (`-std=c11`), compila sin warnings con `-Wall -Wextra -Wpedantic`.
- **Meson** es el único sistema de build. Detecta la plataforma y compila
  `io_event_epoll.c` o `io_event_kqueue.c`, y `watch_inotify.c` o
  `watch_kqueue.c` (ver §8).
- Dependencias: OpenSSL ≥ 3.0, tomlc99 (subproject/wrap), cmocka (solo tests).
- Targets de Meson:
  - `proxy` — el binario del proxy.
  - `test_backend` — servidor backend de pruebas (§10.2).
  - Suites cmocka registradas con `test()`.
- Build `release` (`-Dbuildtype=release`) es el que se usa en el benchmark.
- ASan/UBSan DEBEN poder activarse con `-Db_sanitize=address,undefined` y
  los tests DEBEN pasar con ellos.

## 3. Modelo de procesos y event loop

- Un **proceso master** y **N workers** (`workers = "auto"` → nº de CPUs).
- El master: parsea/valida la config, crea workers, vigila la config,
  reenvía recargas, sirve el endpoint de estadísticas y re-lanza un worker
  que muera inesperadamente.
- Cada worker: un único hilo de event loop + un hilo de health checks + un
  hilo consumidor de log. Ninguna llamada bloqueante en el hilo del loop.
- **Linux**: cada worker abre su propio socket de escucha con
  `SO_REUSEPORT` (el kernel reparte conexiones).
- **macOS**: `SO_REUSEPORT` no reparte carga; el master abre el socket y
  los workers lo heredan (accept compartido). Documentar la diferencia.
- Todos los sockets en modo no bloqueante, **edge-triggered**
  (`EPOLLET` / `EV_CLEAR`): cada handler lee/escribe hasta `EAGAIN`.

### 3.1 API `io_event.h`

```c
io_loop_t *io_loop_create(int max_events);
int  io_loop_add(io_loop_t *, int fd, uint32_t events, void *udata);
int  io_loop_mod(io_loop_t *, int fd, uint32_t events, void *udata);
int  io_loop_del(io_loop_t *, int fd);
int  io_loop_run(io_loop_t *);          /* hasta io_loop_stop() */
void io_loop_stop(io_loop_t *);
void io_loop_destroy(io_loop_t *);
/* eventos: IO_READ, IO_WRITE, IO_ERROR, IO_HUP */
```

Timers: el loop DEBE ofrecer timers (rueda de timers o min-heap) para los
timeouts de §6.4; no se usa un fd por timer.

## 4. Configuración (TOML)

Fichero pasado con `-c <ruta>`. Ejemplo normativo:

```toml
[global]
workers         = "auto"          # o un entero
stats_socket    = "/tmp/proxy.sock"
log_file        = "proxy.log"
log_level       = "info"          # debug|info|warn|error

[timeouts]                        # milisegundos
client_header   = 10000
client_idle     = 60000           # keep-alive ocioso cliente
upstream_connect= 2000
upstream_read   = 30000
upstream_idle   = 30000           # conexión ociosa en el pool

[[frontend]]
name   = "web"
listen = "0.0.0.0:8080"

[[frontend]]
name   = "web-tls"
listen = "0.0.0.0:8443"
tls    = true
certs  = [
  { sni = "*.example.com", cert = "certs/example.crt", key = "certs/example.key" },
  { sni = "api.test",      cert = "certs/api.crt",     key = "certs/api.key" },
]
default_cert = 0                  # índice usado si el SNI no casa

  [[frontend.route]]
  host     = "api.test"
  backend  = "api"

  [[frontend.route]]
  host     = "*.example.com"
  backend  = "web"

  [[frontend.route]]
  host     = "default"
  backend  = "web"

[[backend]]
name        = "api"
strategy    = "least_load"        # round_robin|weighted|least_conn|least_load
max_idle    = 64                  # conexiones ociosas máx. en el pool por servidor
servers = [
  { addr = "127.0.0.1:9001", weight = 1 },
  { addr = "127.0.0.1:9002", weight = 1 },
]
  [backend.health]
  type      = "http"              # tcp|http
  path      = "/health"
  interval  = 2000
  timeout   = 500
  rise      = 2                   # OK consecutivos para volver a "up"
  fall      = 3                   # fallos consecutivos para pasar a "down"
```

Reglas de validación (fallo → el proxy no arranca, o en recarga se descarta
la nueva config y se mantiene la vieja, con error en el log):

- Nombres de frontend y backend únicos; toda `route.backend` existe.
- `listen` válido y sin puertos duplicados.
- Frontend `tls = true` ⇒ al menos un certificado cargable y su clave casa.
- Backend con ≥ 1 servidor; `weight` ≥ 1.
- Como mucho una ruta `default` por frontend.

## 5. Enrutamiento

1. Se toma el `Host:` de la petición (sin puerto, en minúsculas).
   En HTTP/1.1 sin `Host:` → `400`.
2. Resolución dentro del frontend por el que entró la petición:
   **exacto** (hash djb2) → **wildcard** `*.dom` (sufijo más largo gana;
   `*.example.com` casa `a.example.com`, no `example.com`) → **`default`**.
3. Sin ruta → `404 Not Found`. Backend sin servidores sanos → `503`.
4. En frontends TLS: el **SNI selecciona el certificado**; el enrutamiento
   se hace por `Host:`. Si SNI y `Host:` difieren y el `Host:` no está
   cubierto por el certificado servido → `421 Misdirected Request`.
5. Cada petición de una conexión keep-alive se enruta por separado.

## 6. HTTP

### 6.1 Parser y cabeceras

- Parser incremental (acepta datos fragmentados en cualquier byte).
- Cabecera de petición máx. **16 KB** (un slot de `buffer_pool`) → si se
  excede, `431`. Petición malformada → `400`.
- Cabeceras añadidas hacia el backend: `X-Forwarded-For` (concatenando si
  ya existe), `X-Forwarded-Proto` (`http`/`https`), `X-Real-IP`.
- Cabeceras hop-by-hop (`Connection`, `Keep-Alive`, `TE`, `Trailer`,
  `Proxy-Connection` y las listadas en `Connection:`) no se reenvían,
  salvo en el caso de `Upgrade` (§6.5).
- `Content-Length` y `Transfer-Encoding` a la vez → `400`
  (protección frente a request smuggling).

### 6.2 Cuerpos

- `Content-Length`: el cuerpo se reenvía en streaming, sin almacenarlo
  entero.
- `Transfer-Encoding: chunked` en petición y respuesta: se reenvía en
  streaming; el parser sigue los chunks para saber dónde termina el mensaje.
- Respuesta sin longitud (cierre delimita) → se reenvía y se cierra la
  conexión con el cliente.
- Respuestas sin cuerpo: `HEAD`, `1xx`, `204`, `304`.

### 6.3 Keep-alive

- **Cliente↔proxy**: HTTP/1.1 persistente por defecto; HTTP/1.0 solo con
  `Connection: keep-alive`. Se cierra ante `Connection: close`, error, o
  `client_idle`.
- **Proxy↔backend**: pool de conexiones ociosas por servidor (máx.
  `max_idle`). Al terminar una respuesta completa, la conexión vuelve al
  pool; se descarta si el backend envió `Connection: close`, si expira
  `upstream_idle` o si al reutilizarla se detecta cerrada (EOF/`EPOLLRDHUP`).
- Si una conexión reutilizada falla antes de recibir un byte de respuesta
  y la petición es idempotente (`GET`, `HEAD`, `PUT`, `DELETE`, `OPTIONS`),
  se reintenta **una vez** en una conexión nueva.

### 6.4 Pipelining y timeouts

- El proxy acepta peticiones pipelined: las lee y almacena, pero las
  procesa **en serie** (la siguiente se despacha al terminar la respuesta
  anterior), garantizando el orden de las respuestas. Cada una puede ir a
  un backend distinto.
- Timeouts de `[timeouts]`: `client_header` → `408` y cierre;
  `upstream_connect` → marca fallo pasivo y prueba otro servidor, si no
  hay → `502`; `upstream_read` → `504`.

### 6.5 Upgrade / WebSocket

- Petición con `Connection: Upgrade` + `Upgrade:` se reenvía con esas
  cabeceras. Si el backend responde `101`, la conexión pasa a **túnel
  bidireccional** de bytes hasta que un lado cierre; no vuelve al pool.
- Timeout de túnel: `client_idle`.

## 7. Balanceo de carga

| Estrategia | Selección |
|---|---|
| `round_robin` | Siguiente servidor sano en orden |
| `weighted` | Smooth weighted round robin (estilo nginx) |
| `least_conn` | Menos conexiones activas en este worker (empate → RR) |
| `least_load` | Menor carga reportada por el backend (abajo) |

**Indicador de carga (`least_load`)**:

- El backend DEBERÍA añadir `X-Backend-Load: <float 0.0–1.0>` a cada
  respuesta. El proxy lo lee, actualiza la carga del servidor
  (media móvil exponencial, α = 0,3) y **elimina la cabecera** antes de
  reenviar al cliente.
- Valor ausente durante > 5 s o inválido → la carga de ese servidor se
  considera desconocida y se usa `least_conn` como desempate (si también
  empatan, se prefiere el servidor con carga desconocida para refrescarla).
- Para no concentrar todo en un servidor entre actualizaciones: se usa
  "power of two choices" (se eligen dos servidores sanos al azar y gana el
  de menor carga).

**Salud**:

- Pasiva: fallo de connect / reset antes de respuesta cuenta como fallo;
  `fall` fallos consecutivos → `down`.
- Activa: el hilo de health prueba cada `interval` (TCP connect o
  `GET path` esperando `2xx`); `rise` éxitos → `up`.
- Estado compartido entre hilos por atómicos (C11 `<stdatomic.h>`).
- Servidores `down` se excluyen de todas las estrategias.

## 8. Recarga de configuración

Disparadores (ambos DEBEN funcionar):

1. **Automático**: el master vigila el fichero de config
   (`inotify` en Linux; `EVFILT_VNODE` en kqueue). Se debe soportar el
   guardado atómico de editores (rename sobre el fichero), re-armando la
   vigilancia. Debounce de 200 ms.
2. **Manual**: `SIGHUP` al master (self-pipe trick).

Proceso:

- El master parsea y valida. Si falla: log de error, se mantiene la config
  actual, sin afectar al tráfico.
- Si es válida, notifica a los workers; cada uno construye el router y los
  pools nuevos y hace **swap atómico con refcount (RCU)**: las peticiones
  en vuelo terminan con la config vieja, las nuevas usan la nueva.
- Frontends añadidos → se abren; eliminados → dejan de aceptar y sus
  conexiones terminan de forma natural. Certificados TLS se recargan.
- Criterio: durante un `wrk` en curso, una recarga no produce errores de
  socket ni respuestas 5xx atribuibles a la recarga.

## 9. Observabilidad

- **Log**: ring buffer 4096 × 512 B por worker, hilo consumidor que
  escribe al fichero. Si el buffer se llena se descarta el mensaje y se
  incrementa un contador (nunca se bloquea el loop). Access log con:
  timestamp, IP cliente, host, método, ruta, status, backend, latencia.
- **Stats**: el master sirve en `stats_socket` (UNIX) un JSON con uptime,
  conexiones activas, peticiones totales, respuestas por código (2xx…5xx),
  y por servidor backend: estado, conexiones activas, carga reportada,
  fallos. Los workers publican contadores en una región `mmap` compartida.
  Consulta: `nc -U /tmp/proxy.sock` o `socat`.

## 10. Pruebas

### 10.1 Unitarias (cmocka, `meson test`)

Como mínimo: parser HTTP (fragmentación, chunked, límites, smuggling),
router (exacto/wildcard/default), estrategias de balanceo, parser y
validación de config, buffer_pool.

### 10.2 Backend de pruebas (`test_backend`)

Servidor HTTP/1.1 en C sobre la **misma abstracción `io_event`** (kqueue en
macOS, epoll en Linux). Opciones:

```
test_backend --port 9001 --name api-1 [--latency-ms N] [--load F|--load-auto]
             [--threads N] [--drop-every N]
```

- Responde con cuerpo que incluye su `--name` (para verificar el reparto),
  soporta keep-alive, `/health`, eco de cuerpos (`POST /echo`, chunked
  incluido) y WebSocket eco en `/ws`.
- Envía `X-Backend-Load` (fijo con `--load`, o calculado según conexiones
  activas con `--load-auto`).

### 10.3 Integración (`tests/integration/run.sh`)

1. **Genera la configuración**: `tests/gen_config.sh` crea un TOML con
   varios frontends (HTTP y TLS), dominios, backends y estrategias, y
   genera una CA propia + certificados con `openssl`.
2. **Lanza** los `test_backend` necesarios y el proxy.
3. **Dominios**: los tests usan `curl --resolve dominio:puerto:127.0.0.1`
   (sin sudo). Para la demo, `tests/hosts.sh add|remove` inserta/elimina
   un bloque marcado `# BEGIN proxy-test … # END proxy-test` en
   `/etc/hosts` (requiere sudo, idempotente).
4. Casos: enrutamiento por dominio y wildcard, default, 404/503, reparto
   RR y weighted, `least_load` favorece al servidor con menos carga,
   failover al matar un backend, keep-alive (reutilización comprobada en
   stats), pipelining, chunked, WebSocket, SNI/421, recarga automática
   y por SIGHUP, config inválida no tumba el proxy.
5. Limpieza de procesos al terminar, también si un test falla.

## 11. Benchmark

- Herramienta: `wrk` (o `wrk2` para latencia), script `bench/bench_proxy.sh`.
- **Plataforma objetivo: Linux**. Meta: **≥ 50.000 req/s sobre HTTPS**
  (TLS 1.3, keep-alive, sin handshake por petición), `GET` de respuesta
  pequeña (~100 B), 30 s, **0 errores**, contra ≥ 2 `test_backend`.
- Escenarios a reportar: `-t4 -c100`, `-t4 -c200`, `-t4 -c400`, en HTTPS
  (obligatorio) y HTTP (informativo). Reportar req/s, latencia media y p99.
- Documentar hardware, kernel, nº de workers y si se fija CPU con
  `taskset` para separar wrk, proxy y backends.
- macOS: se PUEDE medir y reportar, sin meta obligatoria.

## 12. Entregables

- Código + `meson.build`, tests pasando, `SPEC.md`, `README.md`
  actualizado con resultados reales del benchmark, config de ejemplo y
  vídeo demo.
