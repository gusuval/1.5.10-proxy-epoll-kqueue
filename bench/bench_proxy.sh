#!/usr/bin/env bash
# Benchmark del proxy con wrk (SPEC §11, RNF-01/02).
#
#   bench/bench_proxy.sh
#
# Variables: DURATION (30s), WORKERS (nproc/2), BACKENDS (2),
#            BACKEND_THREADS (2), PIN (1 = fijar CPUs con taskset),
#            TARGET (50000), SCENARIOS ("100 200 400")
#
# Compila en release (build-release/), genera certificados y config,
# lanza BACKENDS test_backend y el proxy, y ejecuta wrk contra HTTPS
# (obligatorio) y HTTP (informativo). Resultados en bench/results/.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build-release
DURATION=${DURATION:-30s}
NCPU=$(nproc)
WORKERS=${WORKERS:-$((NCPU / 2))}
BACKENDS=${BACKENDS:-2}
BACKEND_THREADS=${BACKEND_THREADS:-2}
PIN=${PIN:-1}
TARGET=${TARGET:-50000}
SCENARIOS=${SCENARIOS:-"100 200 400"}
WRK_THREADS=${WRK_THREADS:-4}
PORT_HTTP=38080
PORT_HTTPS=38443

command -v wrk >/dev/null || { echo "falta wrk"; exit 1; }

if [[ ! -d $BUILD ]]; then
    meson setup "$BUILD" "$ROOT" -Dbuildtype=release -Db_lto=true >/dev/null
fi
meson compile -C "$BUILD" >/dev/null

WORK=$(mktemp -d "${TMPDIR:-/tmp}/proxy-bench.XXXXXX")
OUT=$ROOT/bench/results
mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
REPORT=$OUT/bench-$STAMP.md
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# Reparto de CPUs: wrk | proxy | backends
cpus() { local from=$1 n=$2; echo "$from-$((from + n - 1))"; }
if ((PIN)) && command -v taskset >/dev/null && ((NCPU >= WRK_THREADS + WORKERS + 2)); then
    BACK_CPUS=$((NCPU - WRK_THREADS - WORKERS))
    T_WRK="taskset -c $(cpus 0 "$WRK_THREADS")"
    T_PROXY="taskset -c $(cpus "$WRK_THREADS" "$WORKERS")"
    T_BACK="taskset -c $(cpus $((WRK_THREADS + WORKERS)) "$BACK_CPUS")"
    PINNING="wrk: CPUs $(cpus 0 "$WRK_THREADS"), proxy: $(cpus "$WRK_THREADS" "$WORKERS"), backends: $(cpus $((WRK_THREADS + WORKERS)) "$BACK_CPUS")"
else
    T_WRK="" T_PROXY="" T_BACK="" PINNING="sin fijar"
fi

# Certificado del frontend TLS
openssl req -x509 -newkey rsa:2048 -nodes -days 30 -sha256 -keyout "$WORK/bench.key" \
    -out "$WORK/bench.crt" -subj "/CN=bench.test" -addext "subjectAltName=DNS:bench.test" 2>/dev/null

SERVERS=""
for i in $(seq "$BACKENDS"); do SERVERS+="{ addr = \"127.0.0.1:$((39000 + i))\" }, "; done
cat >"$WORK/proxy.toml" <<TOML
[global]
workers      = $WORKERS
stats_socket = "$WORK/proxy.sock"
log_file     = "$WORK/proxy.log"
log_level    = "warn"
access_log   = false

[[frontend]]
name   = "http"
listen = "127.0.0.1:$PORT_HTTP"
  [[frontend.route]]
  host    = "default"
  backend = "bench"

[[frontend]]
name   = "https"
listen = "127.0.0.1:$PORT_HTTPS"
tls    = true
certs  = [ { sni = "bench.test", cert = "bench.crt", key = "bench.key" } ]
  [[frontend.route]]
  host    = "default"
  backend = "bench"

[[backend]]
name     = "bench"
strategy = "round_robin"
servers  = [ ${SERVERS%, } ]
TOML

for i in $(seq "$BACKENDS"); do
    $T_BACK "$BUILD/tools/test_backend" --port $((39000 + i)) --name "b$i" \
        --threads "$BACKEND_THREADS" 2>/dev/null &
    PIDS+=($!)
done
$T_PROXY "$BUILD/src/proxy" -c "$WORK/proxy.toml" 2>"$WORK/proxy.stderr" &
PIDS+=($!)
sleep 1
curl -sf -o /dev/null -H 'Host: bench.test' "http://127.0.0.1:$PORT_HTTP/" || { echo "el proxy no responde"; cat "$WORK/proxy.stderr"; exit 1; }

BODY=$(curl -s -H 'Host: bench.test' "http://127.0.0.1:$PORT_HTTP/" | wc -c)
CPU_MODEL=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //' || sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "?")
MEM=$(free -g 2>/dev/null | awk '/Mem:/{print $2" GB"}' || echo "?")

{
    echo "# Benchmark $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "| Parámetro | Valor |"
    echo "|---|---|"
    echo "| CPU | $CPU_MODEL ($NCPU hilos) |"
    echo "| Memoria | $MEM |"
    echo "| Kernel | $(uname -sr) |"
    echo "| OpenSSL | $(openssl version | cut -d' ' -f2) |"
    echo "| Build | release + LTO |"
    echo "| Workers del proxy | $WORKERS |"
    echo "| Backends | $BACKENDS × test_backend ($BACKEND_THREADS hilos) |"
    echo "| Afinidad | $PINNING |"
    echo "| Petición | GET / (respuesta de $BODY B), keep-alive, $DURATION |"
    echo
    echo "| Configuración | Protocolo | Req/s | Latencia media | p99 | Errores |"
    echo "|---|---|---|---|---|---|"
} >"$REPORT"

FAILED=0
run() { # proto conexiones
    local proto=$1 c=$2 port url
    [[ $proto == HTTPS ]] && url="https://127.0.0.1:$PORT_HTTPS/" || url="http://127.0.0.1:$PORT_HTTP/"
    local log="$OUT/wrk-$STAMP-${proto,,}-c$c.txt"
    echo ">> $proto -t$WRK_THREADS -c$c -d$DURATION" >&2
    $T_WRK wrk -t"$WRK_THREADS" -c"$c" -d"$DURATION" --latency -H 'Host: bench.test' "$url" >"$log" 2>&1
    local rps lat p99 errs non2xx
    rps=$(awk '/Requests\/sec/{print $2}' "$log")
    lat=$(awk '/^    Latency/{print $2}' "$log")
    p99=$(awk '/^ +99%/{print $2}' "$log")
    errs=$(awk '/Socket errors/{s=0; for(i=3;i<=NF;i++) if($i ~ /^[0-9]+,?$/) s+=$i; print s}' "$log")
    non2xx=$(awk '/Non-2xx/{print $NF}' "$log")
    errs=$(( ${errs:-0} + ${non2xx:-0} ))
    printf '| `-t%s -c%s -d%s` | %s | **%s** | %s | %s | %s |\n' "$WRK_THREADS" "$c" "$DURATION" \
        "$proto" "$(printf "%'.0f" "${rps%.*}" 2>/dev/null || echo "$rps")" "$lat" "$p99" "$errs" >>"$REPORT"
    if [[ $proto == HTTPS ]] && { (( ${rps%.*} < TARGET )) || (( errs > 0 )); }; then
        FAILED=1
    fi
    cat "$log" >&2
}

for c in $SCENARIOS; do run HTTPS "$c"; done
run HTTP 200

{
    echo
    if ((FAILED)); then
        echo "**Meta ≥ $TARGET req/s HTTPS con 0 errores: NO alcanzada en algún escenario.**"
    else
        echo "**Meta ≥ $TARGET req/s HTTPS con 0 errores: alcanzada en todos los escenarios.**"
    fi
    echo
    echo "Estadísticas del proxy al terminar:"
    echo
    echo '```json'
    socat - "UNIX-CONNECT:$WORK/proxy.sock" 2>/dev/null | jq '{requests_total, responses, tls_handshakes, connections_accepted, upstream}' 2>/dev/null || true
    echo '```'
} >>"$REPORT"

echo
cat "$REPORT"
echo "informe: $REPORT"
exit $FAILED
