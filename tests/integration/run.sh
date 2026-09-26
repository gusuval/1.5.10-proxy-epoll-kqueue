#!/usr/bin/env bash
# Tests de integración del proxy.
#
#   tests/integration/run.sh [directorio_build]
#
# 1. Genera configuración + CA/certificados (tests/gen_config.sh)
# 2. Lanza los test_backend y el proxy
# 3. Ejecuta los casos (dominios resueltos con --resolve / Host, sin sudo)
# 4. Limpia todos los procesos, también si algo falla
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(cd "${1:-$ROOT/build}" && pwd)
PROXY=$BUILD/src/proxy
BACKEND=$BUILD/tools/test_backend
WS=$BUILD/tools/ws_probe
B=${PORT_BASE:-21000}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/proxy-it.XXXXXX")
HTTP=$((B + 80))
HTTPS=$((B + 443))
ADMIN=$((B + 81))
CFG=$WORK/proxy.toml
CA=$WORK/certs/ca.pem

declare -A BPID
PROXY_PID=
PASS=0
FAIL=0
FAILED=()

cleanup() {
    [[ -n $PROXY_PID ]] && kill "$PROXY_PID" 2>/dev/null
    for p in "${BPID[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    if ((FAIL)) || [[ -n ${KEEP_WORK:-} ]]; then
        echo "directorio de trabajo conservado: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ok() { PASS=$((PASS + 1)); printf '  \e[32mOK\e[0m   %s\n' "$1"; }
ko() { FAIL=$((FAIL + 1)); FAILED+=("$1"); printf '  \e[31mFAIL\e[0m %s %s\n' "$1" "${2:-}"; }
check() { # nombre condición...
    local name=$1; shift
    if "$@"; then ok "$name"; else ko "$name"; fi
}
eq() { [[ "$1" == "$2" ]] || { echo "      esperado '$2', obtenido '$1'" >&2; return 1; }; }
contains() { [[ "$1" == *"$2"* ]] || { echo "      '$2' no aparece en: ${1:0:300}" >&2; return 1; }; }
not_contains() { [[ "$1" != *"$2"* ]] || { echo "      '$2' no debería aparecer" >&2; return 1; }; }
ge() { (( $1 >= $2 )) || { echo "      $1 < $2" >&2; return 1; }; }
section() { printf '\n\e[1m== %s ==\e[0m\n' "$1"; }

# curl contra el frontend HTTP con Host
req() { local host=$1 path=$2; shift 2; curl -s --max-time 10 -H "Host: $host" "$@" "http://127.0.0.1:$HTTP$path"; }
code() { local host=$1 path=$2; shift 2; curl -s -o /dev/null -w '%{http_code}' --max-time 10 -H "Host: $host" "$@" "http://127.0.0.1:$HTTP$path"; }
tls() { local host=$1 path=$2; shift 2; curl -s --max-time 10 --cacert "$CA" --resolve "$host:$HTTPS:127.0.0.1" "$@" "https://$host:$HTTPS$path"; }
stats() { socat - "UNIX-CONNECT:$WORK/proxy.sock" 2>/dev/null; }
stat() { stats | jq -r "$1"; }
# envía texto crudo por TCP y devuelve lo recibido hasta el cierre
raw() { local port=$1 data=$2 t=${3:-5}
    timeout "$t" bash -c 'exec 3<>/dev/tcp/127.0.0.1/'"$port"'; printf "%b" "$1" >&3; cat <&3' _ "$data" 2>/dev/null; }

start_backend() { # puerto nombre args...
    local port=$1 name=$2; shift 2
    "$BACKEND" --port "$port" --name "$name" "$@" 2>>"$WORK/backends.log" &
    BPID[$name]=$!
}
wait_port() {
    for _ in $(seq 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        sleep 0.05
    done
    return 1
}
wait_until() { # timeout_s comando...
    local t=$1; shift
    local end=$((SECONDS + t))
    while ((SECONDS <= end)); do "$@" && return 0; sleep 0.1; done
    return 1
}
count_names() { grep -o "$1" | sort | uniq -c | awk '{print $2"="$1}' | tr '\n' ' '; }
nth() { echo "$1" | tr ' ' '\n' | grep "^$2=" | cut -d= -f2 | grep . || echo 0; }

# ------------------------------------------------------------------
section "preparación"
for bin in "$PROXY" "$BACKEND" "$WS"; do
    [[ -x $bin ]] || { echo "falta $bin (¿meson compile -C build?)"; exit 1; }
done
for t in curl jq socat openssl; do
    command -v $t >/dev/null || { echo "falta la herramienta '$t'"; exit 1; }
done
WORKERS=2 "$ROOT/tests/gen_config.sh" "$WORK" "$B" >/dev/null || exit 1

start_backend $((B + 1001)) api-1
start_backend $((B + 1002)) api-2
start_backend $((B + 1003)) api-3
start_backend $((B + 1004)) web-1
start_backend $((B + 1005)) w-heavy
start_backend $((B + 1006)) w-light
start_backend $((B + 1007)) load-low --load 0.1
start_backend $((B + 1008)) load-high --load 0.9
start_backend $((B + 1009)) lc-slow --latency-ms 400
start_backend $((B + 1010)) lc-fast
start_backend $((B + 1011)) slow --latency-ms 5000
start_backend $((B + 1012)) drop --drop-every 2
for p in $(seq $((B + 1001)) $((B + 1012))); do wait_port "$p" || { echo "backend $p no arranca"; exit 1; }; done

"$PROXY" -c "$CFG" 2>>"$WORK/proxy.stderr" &
PROXY_PID=$!
wait_port $HTTP && wait_port $HTTPS && wait_port $ADMIN || { echo "el proxy no arranca"; cat "$WORK/proxy.log"; exit 1; }
sleep 0.8 # health checks iniciales (rise)
ok "proxy (pid $PROXY_PID) y 12 backends arrancados"

# ------------------------------------------------------------------
section "enrutamiento por dominio (RF-20..23)"
check "dominio exacto api.test -> api"        contains "$(req api.test /)" "hello from api-"
check "wildcard a.example.com -> web"         contains "$(req a.example.com /)" "hello from web-1"
check "wildcard multinivel a.b.example.com"   contains "$(req a.b.example.com /)" "hello from web-1"
check "Host con puerto y mayúsculas"          contains "$(req API.Test:$HTTP /)" "hello from api-"
check "dominio desconocido -> default"        contains "$(req unknown.test /)" "hello from web-1"
check "ápex example.com -> default"           contains "$(req example.com /)" "hello from web-1"
check "frontend sin default -> 404"           eq "$(curl -s -o /dev/null -w '%{http_code}' -H 'Host: nope.test' http://127.0.0.1:$ADMIN/)" 404
check "frontend admin enruta admin.test"      contains "$(curl -s -H 'Host: admin.test' http://127.0.0.1:$ADMIN/)" "api-"
out=$(raw $HTTP 'GET /r1 HTTP/1.1\r\nHost: api.test\r\n\r\nGET /r2 HTTP/1.1\r\nHost: a.example.com\r\nConnection: close\r\n\r\n')
check "cada petición keep-alive se enruta aparte" bash -c '[[ "$1" == *"api-"*"path=/r1"*"web-1 path=/r2"* ]]' _ "$out"

section "errores (RF-22, RF-31, RF-34)"
out=$(raw $HTTP 'GET / HTTP/1.1\r\nAccept: */*\r\n\r\n')
check "HTTP/1.1 sin Host -> 400"              contains "$out" "HTTP/1.1 400"
out=$(raw $HTTP 'GET / HTTP/1.0\r\n\r\n')
check "HTTP/1.0 sin Host -> ruta default"     contains "$out" "hello from web-1"
check "cabecera > 16 KB -> 431"               eq "$(code api.test / -H "X-Big: $(head -c 17000 /dev/zero | tr '\0' a)")" 431
out=$(raw $HTTP 'POST / HTTP/1.1\r\nHost: api.test\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc')
check "Content-Length + Transfer-Encoding -> 400" contains "$out" "HTTP/1.1 400"
out=$(raw $HTTP 'GARBAGE\r\n\r\n')
check "petición malformada -> 400"            contains "$out" "HTTP/1.1 400"
c1=$(code dead.test /)
check "backend caído: primer intento -> 502"  eq "$c1" 502
for _ in $(seq 20); do code dead.test / >/dev/null; done
check "backend sin servidores sanos -> 503"   eq "$(code dead.test /)" 503

section "cabeceras (RF-32, RF-33, RF-55)"
h=$(req api.test /headers -H 'X-Forwarded-For: 1.2.3.4' -H 'Connection: keep-alive, X-Secret' \
     -H 'X-Secret: s3cr3t' -H 'Keep-Alive: timeout=5' -H 'TE: trailers' -H 'X-Real-IP: 6.6.6.6' -H 'X-Keep: yes')
check "X-Forwarded-For concatenado"           contains "$h" "X-Forwarded-For: 1.2.3.4, 127.0.0.1"
check "X-Real-IP reemplazado"                 contains "$h" "X-Real-IP: 127.0.0.1"
check "X-Real-IP del cliente descartado"      not_contains "$h" "6.6.6.6"
check "X-Forwarded-Proto: http"               contains "$h" "X-Forwarded-Proto: http"
check "cabecera end-to-end conservada"        contains "$h" "X-Keep: yes"
check "hop-by-hop Connection eliminada"       not_contains "$h" "Connection:"
check "hop-by-hop Keep-Alive/TE eliminadas"   bash -c '[[ "$1" != *"Keep-Alive:"* && "$1" != *"TE: trailers"* ]]' _ "$h"
check "cabecera listada en Connection eliminada" not_contains "$h" "s3cr3t"
check "X-Forwarded-Proto: https (TLS)"        contains "$(tls api.test /headers)" "X-Forwarded-Proto: https"
hd=$(curl -s -D - -o /dev/null -H 'Host: load.test' http://127.0.0.1:$HTTP/)
check "X-Backend-Load eliminado hacia el cliente" not_contains "${hd,,}" "x-backend-load"
check "resto de cabeceras del backend llegan" contains "$hd" "X-Backend: load-"

section "cuerpos (RF-35..37)"
head -c 10485760 /dev/urandom >"$WORK/10m.bin"
want=$(sha256sum <"$WORK/10m.bin" | cut -d' ' -f1)
got=$(req api.test /echo --data-binary @"$WORK/10m.bin" -H 'Content-Type: application/octet-stream' | sha256sum | cut -d' ' -f1)
check "POST 10 MB Content-Length íntegro"     eq "$got" "$want"
got=$(req api.test /echo --data-binary @"$WORK/10m.bin" -H 'Transfer-Encoding: chunked' | sha256sum | cut -d' ' -f1)
check "POST 10 MB chunked íntegro"            eq "$got" "$want"
got=$(tls api.test /echo --data-binary @"$WORK/10m.bin" | sha256sum | cut -d' ' -f1)
check "POST 10 MB por TLS íntegro"            eq "$got" "$want"
bufs=$(stat .buffers_in_use)
check "buffers liberados tras las subidas (<= 16)" ge 16 "$bufs"
maxrss=0
for pid in $(pgrep -P "$PROXY_PID"); do
    r=$(awk '/VmHWM/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
    ((r > maxrss)) && maxrss=$r
done
check "memoria acotada: pico RSS por worker < 64 MB (${maxrss} kB)" ge 65536 "$maxrss"
check "respuesta chunked"                     eq "$(req x.example.com /chunked | od -c | head -2)" "$(printf 'hello from web-1\n' | od -c | head -2)"
out=$(raw $HTTP 'GET /chunked HTTP/1.1\r\nHost: x.example.com\r\nConnection: close\r\n\r\n')
check "chunked reenviado con trailers"        contains "$out" "X-Trailer: yes"
check "respuesta delimitada por cierre"       contains "$(req x.example.com /close)" "closed-delimited body from web-1"
hd=$(curl -s -I --max-time 5 -H 'Host: x.example.com' http://127.0.0.1:$HTTP/big/1000)
check "HEAD conserva Content-Length sin cuerpo" contains "$hd" "Content-Length: 1000"
check "204 sin cuerpo"                        eq "$(code x.example.com /nocontent)" 204
out=$(raw $HTTP 'HEAD /big/5 HTTP/1.1\r\nHost: x.example.com\r\n\r\nGET /after-head HTTP/1.1\r\nHost: x.example.com\r\nConnection: close\r\n\r\n')
check "HEAD + petición siguiente en la misma conexión" contains "$out" "path=/after-head"
out=$(raw $HTTP 'GET /nocontent HTTP/1.1\r\nHost: x.example.com\r\n\r\nGET /after-204 HTTP/1.1\r\nHost: x.example.com\r\nConnection: close\r\n\r\n')
check "204 + petición siguiente en la misma conexión" contains "$out" "path=/after-204"

section "keep-alive y pool (RF-38..40)"
a0=$(stat .connections_accepted)
curl -s -o /dev/null -H 'Host: api.test' "http://127.0.0.1:$HTTP/k[1-10]"
a1=$(stat .connections_accepted)
check "10 peticiones en 1 conexión cliente"  eq "$((a1 - a0))" 1
hd=$(curl -s -D - -o /dev/null --http1.0 -H 'Host: api.test' http://127.0.0.1:$HTTP/)
check "HTTP/1.0 sin keep-alive -> Connection: close" contains "$hd" "Connection: close"
hd=$(curl -s -D - -o /dev/null --http1.0 -H 'Connection: keep-alive' -H 'Host: api.test' http://127.0.0.1:$HTTP/)
check "HTTP/1.0 keep-alive -> Connection: keep-alive" contains "$hd" "Connection: keep-alive"
# (cada consulta a /stats del backend cuenta como 1 conexión y 1 petición)
s0=$(curl -s "http://127.0.0.1:$((B + 1004))/stats")
curl -s -o /dev/null -H 'Host: x.example.com' "http://127.0.0.1:$HTTP/pool[1-30]"
s1=$(curl -s "http://127.0.0.1:$((B + 1004))/stats")
newc=$(( $(jq .connections <<<"$s1") - $(jq .connections <<<"$s0") - 1 ))
check "30 peticiones reutilizan conexiones al backend (nuevas: $newc)" ge 1 "$newc"
check "el backend recibió las 30 peticiones"  eq "$(( $(jq .requests <<<"$s1") - $(jq .requests <<<"$s0") - 1 ))" 30
check "stats cuentan reutilizaciones"          ge "$(stat .upstream.reuses)" 25
kill "${BPID[web-1]}"; wait "${BPID[web-1]}" 2>/dev/null
start_backend $((B + 1004)) web-1; wait_port $((B + 1004))
fails=0
for i in $(seq 6); do [[ $(code x.example.com /after-restart) == 200 ]] || fails=$((fails + 1)); done
check "conexiones del pool muertas no producen errores" eq "$fails" 0
rt0=$(stat .upstream.retries)
codes=$(curl -s -o /dev/null -w '%{http_code} ' -H 'Host: drop.test' "http://127.0.0.1:$HTTP/d[1-20]")
rt1=$(stat .upstream.retries)
check "reintento de conexión reutilizada que el backend descarta: 20 x 200" eq "$(tr ' ' '\n' <<<"$codes" | grep -c '^200$')" 20
check "reintentos contabilizados ($((rt1 - rt0)))" ge "$((rt1 - rt0))" 5
pc=$(curl -s -o /dev/null -w '%{http_code} ' -H 'Host: drop.test' -X POST -d x "http://127.0.0.1:$HTTP/e[1-2]")
# (el orden depende del worker que atienda cada conexión: basta con que haya un 502)
check "POST no idempotente no se reintenta -> 502 ($pc)" eq "$(tr ' ' '\n' <<<"$pc" | grep . | sort | tr '\n' ' ')" "200 502 "

section "pipelining (RF-41)"
out=$(raw $HTTP 'GET /p1 HTTP/1.1\r\nHost: x.example.com\r\n\r\nGET /p2 HTTP/1.1\r\nHost: x.example.com\r\n\r\nGET /p3 HTTP/1.1\r\nHost: x.example.com\r\nConnection: close\r\n\r\n')
check "3 peticiones pipelined, 3 respuestas en orden" bash -c '[[ "$1" == *"path=/p1"*"path=/p2"*"path=/p3"* ]]' _ "$out"
body=abcdefghij
out=$(raw $HTTP "POST /echo HTTP/1.1\r\nHost: api.test\r\nContent-Length: 10\r\n\r\n${body}GET /p2 HTTP/1.1\r\nHost: api.test\r\nConnection: close\r\n\r\n")
check "pipelining con cuerpo intermedio"      bash -c '[[ "$1" == *"abcdefghij"*"path=/p2"* ]]' _ "$out"

section "timeouts (RF-42)"
check "upstream_read -> 504"                  eq "$(code slow.test /)" 504
out=$(raw $HTTP 'GET / HTTP/1.1\r\nHost: api.test\r\n' 6)
check "client_header incompleta -> 408"       contains "$out" "HTTP/1.1 408"

section "WebSocket (RF-43)"
check "WebSocket por HTTP"                    "$WS" --host api.test 127.0.0.1 $HTTP /ws "hola-ws"
check "WebSocket por TLS"                     "$WS" --tls --ca "$CA" --sni api.test 127.0.0.1 $HTTPS /ws "hola-wss"
check "Upgrade no aceptado -> respuesta normal" contains "$(req api.test /no-ws -H 'Connection: Upgrade' -H 'Upgrade: websocket')" "path=/no-ws"

section "TLS y SNI (RF-11..13)"
subj() { echo | openssl s_client -connect 127.0.0.1:$HTTPS "$@" 2>/dev/null | openssl x509 -noout -subject 2>/dev/null; }
check "SNI api.test -> cert api.test"         contains "$(subj -servername api.test)" "CN=api.test"
check "SNI a.example.com -> cert wildcard"    contains "$(subj -servername a.example.com)" "CN=*.example.com"
check "SNI desconocido -> default_cert"       contains "$(subj -servername zzz.test)" "CN=default.test"
check "sin SNI -> default_cert"               contains "$(subj -noservername)" "CN=default.test"
check "TLS verificado contra la CA"           contains "$(tls api.test /)" "hello from api-"
check "TLS wildcard verificado"               contains "$(tls b.example.com /)" "hello from web-1"
check "TLS 1.3 negociado"                     contains "$(echo | openssl s_client -connect 127.0.0.1:$HTTPS -servername api.test 2>/dev/null)" "TLSv1.3"
check "TLS 1.2 aceptado"                      contains "$(echo | openssl s_client -tls1_2 -connect 127.0.0.1:$HTTPS -servername api.test 2>/dev/null)" "TLSv1.2"
check "SNI distinto de Host no cubierto -> 421" eq "$(curl -s -o /dev/null -w '%{http_code}' --cacert "$CA" --resolve api.test:$HTTPS:127.0.0.1 -H 'Host: other.test' https://api.test:$HTTPS/)" 421
check "SNI distinto de Host cubierto por el cert -> 200" eq "$(curl -s -o /dev/null -w '%{http_code}' --cacert "$CA" --resolve a.example.com:$HTTPS:127.0.0.1 -H 'Host: b.example.com' https://a.example.com:$HTTPS/)" 200

section "balanceo (RF-50..56)"
n=$(curl -s -H 'Host: api.test' "http://127.0.0.1:$HTTP/rr[1-30]" | count_names 'api-[0-9]')
check "round_robin 30 -> 10/10/10 ($n)"       eq "$n" "api-1=10 api-2=10 api-3=10 "
n=$(curl -s -H 'Host: weighted.test' "http://127.0.0.1:$HTTP/w[1-40]" | count_names 'w-[a-z]*')
check "weighted 3:1 -> 30/10 ($n)"            eq "$n" "w-heavy=30 w-light=10 "
n=$(curl -s -H 'Host: load.test' "http://127.0.0.1:$HTTP/l[1-40]" | count_names 'load-[a-z]*')
check "least_load prefiere la carga baja ($n)" ge "$(nth "$n" load-low)" 38
n=$(seq 24 | xargs -P 24 -I{} curl -s -H 'Host: conn.test' "http://127.0.0.1:$HTTP/c{}" | count_names 'lc-[a-z]*')
check "least_conn evita al servidor lento ($n)" ge "$(nth "$n" lc-fast)" 16

section "salud y failover (RF-57..59)"
kill "${BPID[api-2]}"; wait "${BPID[api-2]}" 2>/dev/null
errs=0
for i in $(seq 30); do [[ $(code api.test /f$i) == 200 ]] || errs=$((errs + 1)); done
check "backend caído: 0 errores para el cliente" eq "$errs" 0
check "health activo marca api-2 como down" wait_until 3 bash -c "[[ \$(socat - UNIX-CONNECT:$WORK/proxy.sock | jq -r '.backends[] | select(.server==\"127.0.0.1:$((B + 1002))\") | .state') == down ]]"
start_backend $((B + 1002)) api-2; wait_port $((B + 1002))
check "health activo recupera api-2 (rise)" wait_until 3 bash -c "[[ \$(socat - UNIX-CONNECT:$WORK/proxy.sock | jq -r '.backends[] | select(.server==\"127.0.0.1:$((B + 1002))\") | .state') == up ]]"
n=$(curl -s -H 'Host: api.test' "http://127.0.0.1:$HTTP/back[1-30]" | count_names 'api-[0-9]')
check "api-2 vuelve a recibir tráfico ($n)"  ge "$(nth "$n" api-2)" 5

section "recarga de configuración (RF-62..66)"
ok0=$(stat .reloads_ok); fail0=$(stat .reloads_failed)
sed -i 's|  host = "slow.test"|  host = "new.test"\n  backend = "api"\n  [[frontend.route]]\n  host = "slow.test"|' "$CFG"
check "recarga automática (sed -i, rename) < 1 s" wait_until 1 bash -c "curl -s -H 'Host: new.test' http://127.0.0.1:$HTTP/ | grep -q 'hello from api-'"
cp "$CFG" "$WORK/cfg.bak"
python3 - "$CFG" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read().replace('  host = "new.test"', '  host = "new2.test"')
with open(p, 'r+') as f:  # escritura in situ (como vim con backupcopy=yes)
    f.seek(0); f.write(s); f.truncate()
PY
check "recarga automática (escritura in situ)" wait_until 1 bash -c "curl -s -H 'Host: new2.test' http://127.0.0.1:$HTTP/ | grep -q 'hello from api-'"
ok1=$(stat .reloads_ok)
kill -HUP "$PROXY_PID"
check "recarga manual con SIGHUP"             wait_until 2 bash -c "(( \$(socat - UNIX-CONNECT:$WORK/proxy.sock | jq .reloads_ok) > $ok1 ))"
(curl -s --max-time 10 -H 'Host: api.test' "http://127.0.0.1:$HTTP/sleep/1500" >"$WORK/inflight.out") &
inflight=$!
sleep 0.3
cp "$CFG" "$WORK/good.toml"
echo 'esto no es TOML = = [' >>"$CFG"
check "config inválida rechazada y contada"   wait_until 2 bash -c "(( \$(socat - UNIX-CONNECT:$WORK/proxy.sock | jq .reloads_failed) > $fail0 ))"
check "config inválida: el tráfico sigue"     contains "$(req api.test /)" "hello from api-"
check "config inválida: error en el log"      grep -q "recarga rechazada" "$WORK/proxy.log"
cp "$WORK/good.toml" "$CFG"
sed -i 's|^default_cert = 0|default_cert = 1|' "$CFG"
cat >>"$CFG" <<TOML

[[frontend]]
name   = "extra"
listen = "127.0.0.1:$((B + 82))"
  [[frontend.route]]
  host = "default"
  backend = "api"
TOML
check "frontend añadido en caliente"          wait_until 2 bash -c "curl -s http://127.0.0.1:$((B + 82))/ | grep -q 'hello from api-'"
check "certificados recargados (default_cert)" wait_until 2 bash -c "echo | openssl s_client -connect 127.0.0.1:$HTTPS -noservername 2>/dev/null | openssl x509 -noout -subject | grep -q 'CN=api.test'"
wait $inflight
check "petición en vuelo termina con la config vieja" contains "$(cat "$WORK/inflight.out")" "slept 1500 ms"
cp "$WORK/good.toml" "$CFG"
check "frontend eliminado en caliente"        wait_until 2 bash -c "! curl -s --max-time 1 http://127.0.0.1:$((B + 82))/ >/dev/null"

section "observabilidad (RF-70..72)"
js=$(stats)
check "stats: JSON válido"                    bash -c 'jq -e . >/dev/null <<<"$1"' _ "$js"
check "stats: campos principales"             bash -c 'jq -e ".uptime_s and .requests_total and .responses[\"2xx\"] and (.backends|length>0) and .upstream.connects" >/dev/null <<<"$1"' _ "$js"
check "stats: agregado de ${WORKERS:-2} workers" eq "$(jq .workers_alive <<<"$js")" 2
check "access log con campos"                 grep -qE '\[access\] \[w[0-9]+\] 127\.0\.0\.1 api\.test "GET /" 200 127\.0\.0\.1:[0-9]+ [0-9]+ms' "$WORK/proxy.log"

section "procesos (RF-03)"
w=$(pgrep -P "$PROXY_PID" | head -1)
kill -9 "$w"
check "worker muerto es relanzado"            wait_until 3 bash -c "(( \$(socat - UNIX-CONNECT:$WORK/proxy.sock | jq .worker_restarts) >= 1 && \$(socat - UNIX-CONNECT:$WORK/proxy.sock | jq .workers_alive) == 2 ))"
errs=0
for i in $(seq 20); do [[ $(code api.test /) == 200 ]] || errs=$((errs + 1)); done
check "tráfico normal tras relanzar el worker" eq "$errs" 0

if command -v wrk >/dev/null; then
    section "carga + recargas sin errores (RNF-03)"
    ( for i in $(seq 8); do sleep 0.4; kill -HUP "$PROXY_PID"; done ) &
    out=$(wrk -t2 -c50 -d4s -H 'Host: api.test' "http://127.0.0.1:$HTTP/" 2>&1)
    wait $!
    check "wrk + 8 recargas: sin errores de socket" not_contains "$out" "Socket errors"
    check "wrk + 8 recargas: sin respuestas no-2xx" not_contains "$out" "Non-2xx"
fi

section "parada ordenada"
WPIDS=$(pgrep -P "$PROXY_PID" | tr '\n' ' ')
kill -TERM "$PROXY_PID"
check "SIGTERM: el proxy termina"             wait_until 12 bash -c "! kill -0 $PROXY_PID 2>/dev/null"
wait "$PROXY_PID" 2>/dev/null
check "SIGTERM: sin workers huérfanos ($WPIDS)" bash -c 'for p in $1; do kill -0 $p 2>/dev/null && exit 1; done; exit 0' _ "$WPIDS"
check "SIGTERM: socket de stats eliminado"    test ! -S "$WORK/proxy.sock"
PROXY_PID=

printf '\n\e[1mResultado: %d OK, %d FAIL\e[0m\n' "$PASS" "$FAIL"
for f in "${FAILED[@]}"; do echo "  - $f"; done
((FAIL == 0))
