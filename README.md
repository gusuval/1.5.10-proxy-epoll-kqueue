# 🚄 Proxy Inverso de Alto Rendimiento — C11 + epoll/kqueue

## 🎯 Objetivo del proyecto

Implementar en **C11** un proxy inverso L7 asíncrono capaz de superar **50.000 peticiones/segundo**, portable entre Linux (`epoll`) y macOS (`kqueue`). Es el proyecto de sistemas del curso: aquí no hay framework — se programa directamente contra el sistema operativo.

Con este proyecto el alumno aprende:

- **I/O no bloqueante y event loops**: cómo un solo hilo atiende miles de conexiones (el modelo de nginx y Node.js por dentro).
- La diferencia entre `epoll` (Linux) y `kqueue` (macOS/BSD) y cómo abstraerlas tras una API común.
- Multiproceso con **`SO_REUSEPORT`** (un worker por CPU), balanceo round-robin/weighted/least-conn y health checks.
- Build con **Meson**, tests unitarios con **cmocka** y benchmarking con **wrk**.

## 🏗️ Arquitectura

```
Client ──► [Frontend socket :80/:443] ──► parse HTTP Host header ──► route lookup
        ──► [Backend pool] ──► round-robin ──► upstream connect
        ──► pipe bidireccional ──► close / keep-alive
```

- **Un event-loop por worker**, no bloqueante; edge-triggered (`EPOLLET` / `EV_CLEAR`).
- Capa de abstracción `io_event.[ch]` con la misma API sobre epoll y kqueue.
- Configuración en **TOML** (frontends, rutas por dominio, backends con pesos) con **recarga en caliente vía SIGHUP**.

### Módulos (C11)

| Módulo | Responsabilidad |
|--------|-----------------|
| `io_event` | Abstracción epoll/kqueue (`io_loop_create/add/mod/del/run/stop`) |
| `listener` | Accept loop por puerto frontal, `SO_REUSEPORT` |
| `connection` | Máquina de estados cliente↔upstream, arena de conexiones |
| `http_parser` | Extrae `Host:`; inyecta `X-Forwarded-For/Proto/Real-IP` |
| `router` | Hash djb2 para dominios exactos, wildcards `*.dom`, ruta default, RCU refcount |
| `backend_pool` | round_robin / weighted / least_conn + health pasivo |
| `health` | Sondas TCP/HTTP activas en hilo aparte |
| `config` | Parser TOML (tomlc99), validación, reload |
| `log` | Ring buffer 4096×512B con hilo consumidor |
| `buffer_pool` | Arena `mmap` de slots de 16 KB con freelist |
| `stats` | UNIX socket con snapshot JSON (uptime, contadores, backends) |

## ⚙️ Funcionalidades

- Múltiples frontends (puertos de escucha) y enrutamiento L7 por header `Host:`.
- Resolución de rutas: dominio exacto → wildcard `*.example.com` → `default` → `502`.
- Balanceo `round_robin`, `weighted` y `least_conn` con exclusión de backends caídos.
- Health checks activos (sondas) y pasivos (fallos de conexión).
- **Recarga de configuración sin cortar conexiones** (SIGHUP + swap atómico del router).
- Endpoint de estadísticas JSON.

## 💡 Solución

1. **Edge-triggered obliga a drenar**: con `EPOLLET`/`EV_CLEAR` el kernel avisa una sola vez por cambio; cada handler lee/escribe hasta `EAGAIN`. Es más eficiente pero menos indulgente que level-triggered — el corazón didáctico del proyecto.
2. **Portabilidad por abstracción**: `io_event.h` define la API; `io_event_epoll.c` y `io_event_kqueue.c` la implementan. Meson detecta la plataforma y compila la correcta.
3. **Reload sin downtime**: al recibir SIGHUP (self-pipe trick), se parsea la nueva config en memoria, se valida y se hace un **swap atómico del router** con refcount (RCU) — las conexiones en vuelo terminan con la config vieja.
4. **Logging sin bloquear el event loop**: los mensajes van a un ring buffer y un hilo aparte los escribe a disco.

## 📊 Benchmark (macOS/ARM64, wrk, build release)

| Configuración | Req/s | Latencia media | Errores |
|---|---|---|---|
| `-t4 -c400 -d30s` | **54.183** | 7,39 ms | 0 |
| `-t4 -c200 -d30s` | **52.657** | 3,90 ms | 0 |
| `-t2 -c100 -d30s` | **55.311** | 1,84 ms | 0 |

Meta de ≥ 50.000 req/s **superada** en los tres escenarios.

## 🚀 Cómo ejecutar

```bash
# Build
meson setup build && meson compile -C build

# Tests (22 tests cmocka, 4 suites)
meson test -C build

# Ejecutar con una config TOML
./build/src/proxy -c proxy.toml

# Recargar configuración sin reiniciar
kill -HUP <pid>

# Benchmark
./bench/bench_proxy.sh
```

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
