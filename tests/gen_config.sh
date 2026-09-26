#!/usr/bin/env bash
# Genera una configuración de pruebas completa: CA propia, certificados por
# dominio (SNI) y un proxy.toml con varios frontends HTTP/TLS, rutas por
# dominio exacto/wildcard/default y backends con cada estrategia.
#
#   tests/gen_config.sh <directorio> [puerto_base]
#
# Puertos (base B = 18000 por defecto):
#   B+80  frontend HTTP  "web"        B+443 frontend TLS "web-tls"
#   B+81  frontend HTTP  "admin" (sin ruta default -> 404)
#   B+1001..B+1012 servidores test_backend (ver lista de backends abajo)
set -euo pipefail

OUT=${1:?uso: gen_config.sh <directorio> [puerto_base]}
B=${2:-18000}
mkdir -p "$OUT/certs"
C="$OUT/certs"

if [[ ! -f "$C/ca.pem" ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
        -keyout "$C/ca.key" -out "$C/ca.pem" -subj "/CN=proxy-test-ca" 2>/dev/null
fi

mkcert() { # nombre fichero san...
    local name=$1 file=$2; shift 2
    [[ -f "$C/$file.crt" ]] && return
    local san=""
    for d in "$@"; do san+="DNS:$d,"; done
    san=${san%,}
    openssl req -newkey rsa:2048 -nodes -sha256 -keyout "$C/$file.key" \
        -out "$C/$file.csr" -subj "/CN=$name" 2>/dev/null
    openssl x509 -req -in "$C/$file.csr" -CA "$C/ca.pem" -CAkey "$C/ca.key" \
        -CAcreateserial -days 3650 -sha256 -out "$C/$file.crt" \
        -extfile <(printf "subjectAltName=%s\nextendedKeyUsage=serverAuth\n" "$san") 2>/dev/null
    rm -f "$C/$file.csr"
}
mkcert "default.test" default default.test
mkcert "api.test" api api.test
mkcert "*.example.com" wild-example "*.example.com"

cat > "$OUT/proxy.toml" <<TOML
# Generado por tests/gen_config.sh
[global]
workers      = ${WORKERS:-2}
stats_socket = "$OUT/proxy.sock"
log_file     = "$OUT/proxy.log"
log_level    = "info"
access_log   = true

[timeouts]
client_header    = 3000
client_idle      = 10000
upstream_connect = 1000
upstream_read    = 3000
upstream_idle    = 5000

[[frontend]]
name   = "web"
listen = "127.0.0.1:$((B + 80))"

  [[frontend.route]]
  host = "api.test"
  backend = "api"
  [[frontend.route]]
  host = "*.example.com"
  backend = "web"
  [[frontend.route]]
  host = "weighted.test"
  backend = "weighted"
  [[frontend.route]]
  host = "load.test"
  backend = "loadbal"
  [[frontend.route]]
  host = "conn.test"
  backend = "leastconn"
  [[frontend.route]]
  host = "dead.test"
  backend = "dead"
  [[frontend.route]]
  host = "slow.test"
  backend = "slow"
  [[frontend.route]]
  host = "drop.test"
  backend = "drop"
  [[frontend.route]]
  host = "default"
  backend = "web"

[[frontend]]
name   = "web-tls"
listen = "127.0.0.1:$((B + 443))"
tls    = true
default_cert = 0
certs  = [
  { sni = "default.test",  cert = "certs/default.crt",      key = "certs/default.key" },
  { sni = "api.test",      cert = "certs/api.crt",          key = "certs/api.key" },
  { sni = "*.example.com", cert = "certs/wild-example.crt", key = "certs/wild-example.key" },
]

  [[frontend.route]]
  host = "api.test"
  backend = "api"
  [[frontend.route]]
  host = "*.example.com"
  backend = "web"
  [[frontend.route]]
  host = "default"
  backend = "web"

[[frontend]]
name   = "admin"
listen = "127.0.0.1:$((B + 81))"

  [[frontend.route]]
  host = "admin.test"
  backend = "api"

[[backend]]
name     = "api"
strategy = "round_robin"
servers  = [
  { addr = "127.0.0.1:$((B + 1001))" },
  { addr = "127.0.0.1:$((B + 1002))" },
  { addr = "127.0.0.1:$((B + 1003))" },
]
  [backend.health]
  type     = "http"
  path     = "/health"
  interval = 300
  timeout  = 200
  rise     = 2
  fall     = 2

[[backend]]
name     = "web"
strategy = "round_robin"
servers  = [ { addr = "127.0.0.1:$((B + 1004))" } ]

[[backend]]
name     = "weighted"
strategy = "weighted"
servers  = [
  { addr = "127.0.0.1:$((B + 1005))", weight = 3 },
  { addr = "127.0.0.1:$((B + 1006))", weight = 1 },
]

[[backend]]
name     = "loadbal"
strategy = "least_load"
servers  = [
  { addr = "127.0.0.1:$((B + 1007))" },
  { addr = "127.0.0.1:$((B + 1008))" },
]

[[backend]]
name     = "leastconn"
strategy = "least_conn"
servers  = [
  { addr = "127.0.0.1:$((B + 1009))" },
  { addr = "127.0.0.1:$((B + 1010))" },
]

[[backend]]
name     = "dead"
servers  = [ { addr = "127.0.0.1:$((B + 1099))" } ]

[[backend]]
name     = "slow"
servers  = [ { addr = "127.0.0.1:$((B + 1011))" } ]

[[backend]]
name     = "drop"
servers  = [ { addr = "127.0.0.1:$((B + 1012))" } ]
TOML

echo "configuración generada en $OUT/proxy.toml (CA: $C/ca.pem)"
