# Requerimientos — Proxy inverso L7 en C11 (epoll/kqueue)

Catálogo de requerimientos verificables derivado de [`PROMPT.md`](../PROMPT.md)
y detallado en [`SPEC.md`](../SPEC.md). Cada requerimiento tiene un
identificador, prioridad, criterio de aceptación y la sección de la spec
donde se detalla.

**Prioridad**: **M** = obligatorio (must), **S** = recomendado (should),
**C** = opcional (could).

**Verificación**: **UT** = test unitario cmocka, **IT** = test de
integración, **BM** = benchmark, **RV** = revisión de código/documentación.

---

## 1. Requerimientos funcionales

### 1.1 Event loop y plataforma

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-01 | El proxy usa `epoll` en Linux y `kqueue` en macOS/BSD, tras una API común `io_event.h`. | M | Meson compila `io_event_epoll.c` en Linux y `io_event_kqueue.c` en macOS; el resto del código no incluye cabeceras de epoll/kqueue. | RV, IT | §3.1 |
| RF-02 | Toda la E/S es no bloqueante y edge-triggered (`EPOLLET` / `EV_CLEAR`), drenando hasta `EAGAIN`. | M | Revisión: cada handler de lectura/escritura itera hasta `EAGAIN`; test con cuerpos > 16 KB sin cuelgues. | RV, IT | §3 |
| RF-03 | Modelo master + N workers (`workers = "auto"` → nº CPUs). El master re-lanza un worker que muere. | M | `kill -9` a un worker → el master lanza otro y el tráfico continúa. | IT | §3 |
| RF-04 | En Linux cada worker escucha con `SO_REUSEPORT`; en macOS los workers comparten el socket del master. | M | Stats muestran peticiones repartidas entre todos los workers en ambas plataformas. | IT | §3 |
| RF-05 | El event loop ofrece timers sin usar un fd por timer. | M | Test unitario de la rueda/heap de timers (orden, cancelación). | UT | §3.1 |
| RF-06 | Windows / IOCP queda fuera de alcance. | — | No aplica. | — | §1 |

### 1.2 Frontends y TLS

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-10 | Se pueden definir varios frontends escuchando en puertos distintos. | M | Config con ≥ 2 frontends; ambos responden y tienen rutas independientes. | IT | §4 |
| RF-11 | Un frontend puede terminar TLS (OpenSSL ≥ 3.0, TLS 1.2 y 1.3). | M | `curl --cacert ca.pem https://…` responde 200. | IT | §4, §5 |
| RF-12 | Un frontend TLS admite varios certificados y los elige por SNI, con `default_cert` si no hay coincidencia. | M | `openssl s_client -servername X` recibe el certificado de X; SNI desconocido recibe el de por defecto. | IT | §5 |
| RF-13 | Si el `Host:` no está cubierto por el certificado servido por SNI → `421`. | S | Petición con SNI `a.test` y `Host: b.test` → `421`. | IT | §5 |
| RF-14 | La conexión proxy→backend es HTTP en claro. | M | Revisión de código. | RV | §1 |

### 1.3 Enrutamiento por dominio

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-20 | Cada frontend enruta por la cabecera `Host:` (sin puerto, insensible a mayúsculas) hacia un backend. | M | Dos dominios en el mismo puerto llegan a backends distintos (identificados por nombre en la respuesta). | UT, IT | §5 |
| RF-21 | Orden de resolución: exacto → wildcard `*.dom` (sufijo más largo gana) → `default`. | M | `a.example.com` casa `*.example.com`; `example.com` no; wildcard más específico gana. | UT | §5 |
| RF-22 | Sin ruta → `404`; backend sin servidores sanos → `503`; HTTP/1.1 sin `Host:` → `400`. | M | Casos de test para cada código. | IT | §5 |
| RF-23 | En keep-alive, cada petición de la conexión se enruta por separado. | M | Dos peticiones con `Host` distinto en la misma conexión llegan a backends distintos. | IT | §5 |

### 1.4 HTTP/1.1

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-30 | Parser HTTP incremental que tolera la fragmentación en cualquier byte. | M | Test unitario que alimenta la petición byte a byte y en trozos aleatorios. | UT | §6.1 |
| RF-31 | Límite de cabecera 16 KB → `431`; petición malformada → `400`. | M | Tests con cabecera de 17 KB y con línea de petición inválida. | UT, IT | §6.1 |
| RF-32 | Añade `X-Forwarded-For` (concatenando), `X-Forwarded-Proto` y `X-Real-IP` hacia el backend. | M | El backend de eco devuelve las cabeceras con los valores correctos. | IT | §6.1 |
| RF-33 | No reenvía cabeceras hop-by-hop (salvo en Upgrade). | M | El backend de eco no recibe `Connection`/`Keep-Alive`/`TE`. | IT | §6.1 |
| RF-34 | Rechaza `Content-Length` + `Transfer-Encoding` a la vez con `400`. | M | Test unitario e integración. | UT, IT | §6.1 |
| RF-35 | Reenvía en streaming cuerpos con `Content-Length`. | M | `POST /echo` de 10 MB devuelve el mismo hash; memoria del proxy acotada. | IT | §6.2 |
| RF-36 | Soporta `Transfer-Encoding: chunked` en petición y respuesta. | M | `POST /echo` chunked devuelve el mismo contenido; respuestas chunked llegan íntegras. | UT, IT | §6.2 |
| RF-37 | Respuestas sin cuerpo (`HEAD`, `1xx`, `204`, `304`) y respuestas delimitadas por cierre. | M | Tests de cada caso sin cuelgues. | IT | §6.2 |
| RF-38 | Keep-alive cliente↔proxy: persistente en 1.1 por defecto, en 1.0 solo con `Connection: keep-alive`. | M | Varias peticiones por la misma conexión TCP (`curl -v` muestra reutilización). | IT | §6.3 |
| RF-39 | Pool de conexiones ociosas proxy→backend por servidor (`max_idle`, `upstream_idle`). | M | Tras N peticiones secuenciales, el backend ve 1 sola conexión; stats muestran reutilización. | IT | §6.3 |
| RF-40 | Reintento único en conexión nueva si una conexión reutilizada falla antes de la respuesta y el método es idempotente. | S | El backend cierra conexiones ociosas; el cliente no ve errores en `GET`. | IT | §6.3 |
| RF-41 | Pipelining: acepta varias peticiones en vuelo y las responde en orden. | M | 3 peticiones enviadas en un único `write` → 3 respuestas en orden. | IT | §6.4 |
| RF-42 | Timeouts: `client_header` → `408`, `upstream_connect` → otro servidor o `502`, `upstream_read` → `504`, `client_idle` cierra. | M | Test por timeout con `test_backend --latency-ms`. | IT | §6.4 |
| RF-43 | Upgrade/WebSocket: tras `101` la conexión pasa a túnel bidireccional y no vuelve al pool. | M | Cliente WebSocket envía y recibe eco a través del proxy (HTTP y TLS). | IT | §6.5 |

### 1.5 Balanceo de carga y salud

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-50 | Cada backend tiene n servidores; estrategias `round_robin`, `weighted`, `least_conn` y `least_load`. | M | Tests unitarios de la secuencia de selección de cada estrategia. | UT | §7 |
| RF-51 | `round_robin` reparte en orden entre servidores sanos. | M | 300 peticiones a 3 servidores → 100 ± 1 cada uno. | IT | §7 |
| RF-52 | `weighted` usa smooth weighted round robin. | M | Pesos 3:1 → reparto 75 % / 25 % ± 2 % y sin ráfagas consecutivas largas. | UT, IT | §7 |
| RF-53 | `least_conn` elige el servidor con menos conexiones activas (empate → RR). | M | Con un backend lento, las peticiones nuevas van al rápido. | UT, IT | §7 |
| RF-54 | `least_load` usa la cabecera `X-Backend-Load` (0.0–1.0), suavizada con EMA α = 0,3 y selección "power of two choices". | M | Backends con `--load 0.1` y `--load 0.9` → el primero recibe claramente más tráfico. | UT, IT | §7 |
| RF-55 | El proxy elimina `X-Backend-Load` antes de responder al cliente. | M | La respuesta al cliente no contiene la cabecera. | IT | §7 |
| RF-56 | Carga ausente > 5 s o inválida → desconocida; se desempata con `least_conn`. | S | Test unitario con reloj simulado. | UT | §7 |
| RF-57 | Health pasivo: `fall` fallos consecutivos → servidor `down`. | M | Matar un backend → deja de recibir tráfico sin errores al cliente tras `fall` intentos. | IT | §7 |
| RF-58 | Health activo TCP/HTTP en hilo aparte; `rise` éxitos → `up`. | M | Relanzar el backend → vuelve a recibir tráfico en ≤ `rise × interval`. | IT | §7 |
| RF-59 | Servidores `down` se excluyen de todas las estrategias. | M | Test unitario por estrategia. | UT | §7 |

### 1.6 Configuración y recarga

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-60 | Configuración en TOML (`-c <ruta>`) con el esquema de la spec. | M | La config de ejemplo arranca el proxy. | IT | §4 |
| RF-61 | Validación: nombres únicos, rutas a backends existentes, puertos sin duplicar, certificados cargables, ≥ 1 servidor por backend, una sola ruta `default` por frontend. | M | Un test por regla: config inválida → el proxy no arranca y explica el error. | UT | §4 |
| RF-62 | Recarga automática al modificar el fichero (inotify / `EVFILT_VNODE`), incluido guardado por rename, con debounce de 200 ms. | M | Editar el fichero con `vim` y con `sed -i` → la nueva ruta funciona en < 1 s. | IT | §8 |
| RF-63 | Recarga manual con `SIGHUP` al master (self-pipe trick). | M | `kill -HUP` → la nueva config aplica. | IT | §8 |
| RF-64 | Config inválida en recarga → se conserva la anterior y se registra el error. | M | Guardar TOML roto → el tráfico sigue sin errores; log con el error. | IT | §8 |
| RF-65 | Swap atómico del router con refcount (RCU): peticiones en vuelo terminan con la config vieja. | M | Petición lenta en curso durante la recarga termina con 200. | IT | §8 |
| RF-66 | Recarga abre frontends nuevos, cierra los eliminados y recarga certificados. | M | Añadir puerto → escucha; quitarlo → deja de aceptar; cambiar cert → nuevo cert servido. | IT | §8 |

### 1.7 Observabilidad

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-70 | Log asíncrono: ring buffer 4096 × 512 B por worker con hilo consumidor; si se llena, descarta y cuenta. | M | Revisión; test unitario del ring buffer (lleno, vacío, concurrencia). | UT, RV | §9 |
| RF-71 | Access log con timestamp, IP, host, método, ruta, status, backend y latencia. | M | Línea de log por petición con todos los campos. | IT | §9 |
| RF-72 | Endpoint de estadísticas JSON por socket UNIX, agregado de todos los workers. | M | `nc -U /tmp/proxy.sock` devuelve JSON válido (`jq .`) con los campos de la spec. | IT | §9 |

### 1.8 Herramientas de prueba

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RF-80 | `test_backend` en C sobre `io_event` (kqueue en macOS, epoll en Linux) con `--port`, `--name`, `--latency-ms`, `--load`/`--load-auto`. | M | Compila y funciona en ambas plataformas. | IT | §10.2 |
| RF-81 | `test_backend` soporta keep-alive, `/health`, `POST /echo` (incluido chunked) y WebSocket eco en `/ws`, y envía `X-Backend-Load`. | M | Tests de integración usan cada endpoint. | IT | §10.2 |
| RF-82 | `tests/gen_config.sh` genera config TOML (varios frontends HTTP/TLS, dominios, backends, estrategias) y una CA + certificados con `openssl`. | M | El script genera ficheros con los que el proxy arranca. | IT | §10.3 |
| RF-83 | `tests/integration/run.sh` lanza backends y proxy, ejecuta los casos y limpia procesos incluso si falla. | M | Tras un fallo forzado no quedan procesos (`pgrep`). | IT | §10.3 |
| RF-84 | Los tests usan `curl --resolve` (sin sudo); `tests/hosts.sh add\|remove` gestiona un bloque marcado en `/etc/hosts` de forma idempotente. | M | `add` dos veces no duplica; `remove` deja `/etc/hosts` como estaba. | IT | §10.3 |

---

## 2. Requerimientos no funcionales

| ID | Requerimiento | Prio. | Criterio de aceptación | Verif. | Spec |
|---|---|---|---|---|---|
| RNF-01 | **Rendimiento**: ≥ 50.000 req/s sobre HTTPS (TLS 1.3, keep-alive) en Linux, `GET` ~100 B, 30 s, 0 errores, ≥ 2 backends. | M | `bench/bench_proxy.sh` con `-t4 -c100/-c200/-c400` alcanza la meta en HTTPS. | BM | §11 |
| RNF-02 | El benchmark reporta req/s, latencia media y p99, en HTTPS y HTTP, con hardware, kernel y nº de workers. | M | Tabla completa en el README. | RV | §11 |
| RNF-03 | **Disponibilidad**: una recarga durante un `wrk` en curso no produce errores de socket ni 5xx. | M | `wrk` + recargas repetidas → 0 errores. | BM | §8 |
| RNF-04 | **No bloqueo**: ninguna llamada bloqueante en el hilo del event loop (DNS, disco, health). | M | Revisión de código. | RV | §3 |
| RNF-05 | **Memoria acotada**: buffers de 16 KB de un `buffer_pool` (`mmap` + freelist); los cuerpos no se almacenan enteros. | M | Memoria RSS estable durante el benchmark y con cuerpos de 10 MB. | IT, BM | §6.2 |
| RNF-06 | **Robustez**: sin fugas ni UB; los tests pasan con ASan/UBSan. | M | `meson test` con `-Db_sanitize=address,undefined` en verde. | UT, IT | §2 |
| RNF-07 | **Seguridad**: protección frente a request smuggling y cabeceras sobredimensionadas. | M | RF-31 y RF-34 verificados. | UT | §6.1 |
| RNF-08 | **Portabilidad**: compila y pasa los tests en Linux y macOS. | M | `meson test` verde en ambas. | UT, IT | §2 |
| RNF-09 | **Calidad de código**: C11, sin warnings con `-Wall -Wextra -Wpedantic`, módulos según la tabla del README. | M | Build limpio. | RV | §2 |
| RNF-10 | **Build**: Meson como único sistema de build; dependencias OpenSSL ≥ 3.0, tomlc99 (wrap) y cmocka. | M | `meson setup build && meson compile -C build` desde cero. | RV | §2 |

---

## 3. Entregables

| ID | Entregable | Prio. |
|---|---|---|
| EN-01 | Código fuente + `meson.build` | M |
| EN-02 | Tests unitarios e integración en verde | M |
| EN-03 | `SPEC.md`, `docs/requerimientos.md` y `README.md` con resultados reales del benchmark | M |
| EN-04 | Configuración de ejemplo (`proxy.toml`) | M |
| EN-05 | Vídeo demo | M |

---

## 4. Trazabilidad con `PROMPT.md`

| Punto de `PROMPT.md` | Requerimientos |
|---|---|
| epoll / kqueue / IOCP | RF-01, RF-02, RF-06 |
| Redirección por nombre de dominio | RF-20 – RF-23 |
| Proxy nivel 7 | RF-30 – RF-43 |
| Varias entradas en distintos puertos | RF-10 – RF-12 |
| n salidas por entrada según dominio | RF-20, RF-21 |
| Round robin o balanceo por indicadores de carga | RF-50 – RF-59 |
| Keep-alive HTTP/1.1 | RF-38 – RF-40 |
| Proyecto con Meson | RNF-10 |
| Recarga automática de la configuración | RF-62 – RF-66 |
| Test: generar config, lanzar servidores, `/etc/hosts` | RF-80 – RF-84 |
| Benchmark 50.000 req/s | RNF-01 – RNF-03 |
| Backend de prueba en C con kqueue | RF-80, RF-81 |
