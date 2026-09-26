# Decisiones de diseño

Cada decisión con la alternativa descartada y el motivo.

## Procesos y event loop

**Master + N workers, un hilo de event loop por worker.**
Alternativa: un proceso con N hilos de event loop. Con procesos, un fallo de memoria en un worker no tumba el proxy (el master lo relanza) y no hay que sincronizar nada entre loops. Lo que sí se comparte (estadísticas) va por una región `mmap` con contadores atómicos.

**`SO_REUSEPORT` por worker en Linux; sockets heredados del master en macOS/BSD.**
En Linux el kernel reparte las conexiones entre los sockets del grupo, sin *thundering herd*. En macOS `SO_REUSEPORT` permite el bind, pero no reparte: entrega las conexiones a un único socket. Allí el master abre los sockets y los pasa a los workers por el canal (`SCM_RIGHTS`), también en las recargas que añaden frontends.

**Edge-triggered, cada fd registrado una sola vez con lectura+escritura.**
Alternativa: level-triggered, o edge-triggered activando `EPOLLOUT` solo cuando hace falta. Registrar una vez y llevar dos flags por fd (`rd`/`wr`: "el último intento no dio EAGAIN") evita un `epoll_ctl(MOD)` por cambio de estado. `sess_drive()` repite leer → procesar → escribir mientras haya progreso. Así nunca queda trabajo pendiente sin un evento que lo despierte, que es el error típico con ET. Una sesión que agota su presupuesto (32 vueltas) se replanifica al final de la iteración para no acaparar el loop.

**Liberación diferida.**
Cerrar un fd y hacer `free()` del objeto mientras hay más eventos suyos en el mismo lote de `epoll_wait` es un use-after-free. Los objetos se marcan `closed`, los eventos tardíos se ignoran y la memoria se libera en `io_loop_defer`, al final de la iteración.

**Timers en min-heap con actualización perezosa.**
Cada byte recibido "refresca" un timeout. Retrasar un deadline solo actualiza un campo (O(1)); el timer se reubica al vencer. Solo adelantarlo reordena el heap. Sin un fd por timer.

**Reloj cacheado por iteración, actualizado antes de despachar.**
Un fallo real encontrado por los tests: si el reloj se actualiza después de despachar los eventos, un handler que llega tras un periodo ocioso calcula deadlines ya vencidos, y aparecen 408/504 espurios. Hay un test de regresión (`test_now_fresh_in_handler`).

## HTTP

**La cabecera se parsea cuando está completa; el cuerpo lo delimita una máquina de estados aparte.**
Alternativa: un parser byte a byte con estado para todo (tipo `http-parser` de Node). Buscar `\r\n\r\n`, recordando hasta dónde se ha escaneado, tolera cualquier fragmentación sin reescanear. Después la cabecera se parsea de una vez, con punteros al buffer y sin copias. El cuerpo (`Content-Length`, chunked con extensiones y trailers, o delimitado por cierre) solo se sigue para saber dónde termina: los bytes se reenvían tal cual.

**Cuatro buffers de 16 KB por sesión y copia en espacio de usuario.**
Alternativa: `splice()`/zero-copy. Con TLS los datos pasan por espacio de usuario de todos modos y las cabeceras hay que reescribirlas. Los buffers salen de un pool `mmap` con freelist y se devuelven en cuanto la conexión queda ociosa: una conexión keep-alive inactiva no retiene buffers. La cabecera máxima es de 16 KB (un slot); si la cabecera reescrita no cabe, se responde 431.

**Pipelining en serie.**
Las peticiones pipelined se leen por adelantado (hasta llenar el buffer), pero se despachan de una en una: la siguiente se despacha cuando la anterior se ha terminado de recibir del backend. El orden de las respuestas queda garantizado sin colas y cada petición puede ir a un backend distinto.

**Errores locales con `Connection: close`.**
Las respuestas generadas por el proxy (400, 404, 408, 421, 431, 502, 503, 504) cierran la conexión. Tras un error puede quedar un cuerpo a medio leer, y cerrar es lo seguro. El cierre es "con linger": se vacía la salida, `shutdown(SHUT_WR)` y se descarta lo que siga llegando durante 2 s, para que un RST no se coma la respuesta.

**Reintento solo en un caso muy acotado.**
Una conexión del pool puede estar muerta sin que el proxy lo sepa todavía. Se reintenta una vez, en una conexión nueva, solo si la conexión era reutilizada, no llegó ni un byte de respuesta y la petición es idempotente y sin cuerpo. Para eso se conserva la cabecera enviada en el buffer. Un POST nunca se reintenta: el backend podría haberlo procesado.

**Protección frente a request smuggling.**
Se rechazan `Content-Length` + `Transfer-Encoding`, varios `Content-Length` distintos, TE sin `chunked` final, obs-fold y espacios antes de `:`. Las cabeceras nombradas en `Connection:` se eliminan, excepto `Host`, `Content-Length` y `Transfer-Encoding` (si no, se podría forzar al proxy a quitar el encuadre).

## Balanceo y salud

**`least_load`: EMA + power of two choices.**
La carga llega en `X-Backend-Load` con cada respuesta, se suaviza con una media móvil (α = 0,3) y caduca a los 5 s. Elegir siempre el mínimo haría que todos los workers se lanzaran a la vez sobre el mismo servidor entre dos actualizaciones. Con P2C (dos candidatos al azar, gana el de menos carga) se reparte mejor y sigue favoreciendo al menos cargado. Si la carga es desconocida o hay empate, decide `least_conn`. Con empate también en conexiones, gana el servidor sin carga fresca: así se conoce su carga cuanto antes. Con el desempate aleatorio inicial, un test de integración resultó inestable (35/40 en vez de ≥ 38).

**Estado de salud por worker.**
Cada worker tiene su hilo de sondas, sin compartir estado con los demás. Es más simple y no hay locks entre procesos, a cambio de multiplicar las sondas por el número de workers. Las estadísticas muestran `up_workers` y un estado `degraded` si los workers discrepan.

**Recuperación pasiva half-open.**
Sin health activo, un servidor caído vuelve a probarse pasado `interval`. Si falla otra vez, cae con un solo fallo, sin esperar otros `fall`.

## Configuración y recarga

**El master envía el texto de la config a los workers, en vez de que cada uno relea el fichero.**
Así todos los workers aplican exactamente la versión que validó el master, aunque el fichero cambie mientras tanto. El master descarta además los eventos del watcher que no cambian el contenido.

**Generaciones con refcount (RCU sin atómicos).**
Una generación (`runtime_t`) agrupa routers, contextos TLS, backends y su hilo de health. Cada petición toma una referencia al empezar y la suelta al terminar; el swap es cambiar un puntero. Todo pasa en el hilo del loop, así que el refcount no necesita atómicos. Una generación retirada se libera cuando llega a 0 referencias y su hilo de health ha terminado (se comprueba en el tick de 250 ms).

**inotify sobre el directorio, no sobre el fichero.**
vim y `sed -i` guardan escribiendo un fichero nuevo y renombrándolo encima. Un watch sobre el fichero se perdería; uno sobre el directorio, filtrando por nombre, cubre la escritura in situ y el rename. En kqueue se vigilan fichero y directorio, y se reabre el fichero al detectar un reemplazo.

**Validación estricta.**
Las claves desconocidas son un error (`workerz = 2` no se ignora en silencio). Una config inválida al arrancar impide el arranque; en una recarga se descarta y se mantiene la anterior.

## TLS

**Un `SSL_CTX` por certificado y cambio en el callback de SNI.**
Es el mecanismo estándar de OpenSSL. SNI exacto, luego wildcard de una sola etiqueta (RFC 6125) y, si no, `default_cert`. La comprobación 421 (SNI ≠ Host) usa el patrón `sni` de la config, no los SAN del certificado.

## Limitaciones conocidas

- Un cliente HTTP/1.0 recibe tal cual una respuesta chunked del backend; no se convierte a longitud fija.
- Cambiar `workers` requiere reiniciar (se avisa en el log).
- Durante una recarga, cada worker lee los certificados del disco en el hilo del loop. Son milisegundos, y solo al recargar.
- `upstream_connect` no se prueba automáticamente: en loopback un connect rechazado falla al instante y no hay timeout.
- macOS/BSD: el código kqueue no se ha ejecutado en este entorno (ver el README).
