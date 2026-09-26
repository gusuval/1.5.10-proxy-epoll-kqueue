#!/usr/bin/env bash
# Da de alta / baja en /etc/hosts los dominios de prueba (para la demo).
# Los tests automáticos no lo necesitan: usan curl --resolve / Host.
#
#   sudo tests/hosts.sh add       # añade el bloque (idempotente)
#   sudo tests/hosts.sh remove    # lo elimina y deja el fichero como estaba
#   tests/hosts.sh show
#
# HOSTS_FILE permite apuntar a otro fichero (útil para probar el script).
set -euo pipefail

HOSTS=${HOSTS_FILE:-/etc/hosts}
BEGIN="# BEGIN proxy-test"
END="# END proxy-test"
DOMAINS=(api.test admin.test default.test weighted.test load.test conn.test
         dead.test slow.test new.test www.example.com a.example.com b.example.com)

strip() { sed "/^$BEGIN\$/,/^$END\$/d" "$HOSTS"; }

write() { # contenido nuevo por stdin, preservando permisos/propietario
    local tmp
    tmp=$(mktemp)
    cat >"$tmp"
    if [[ -w $HOSTS ]]; then
        cat "$tmp" >"$HOSTS"
    else
        sudo tee "$HOSTS" <"$tmp" >/dev/null
    fi
    rm -f "$tmp"
}

case ${1:-} in
add)
    { strip
      echo "$BEGIN"
      for d in "${DOMAINS[@]}"; do printf '127.0.0.1\t%s\n' "$d"; done
      echo "$END"; } | write
    echo "añadidos ${#DOMAINS[@]} dominios a $HOSTS"
    ;;
remove)
    strip | write
    echo "bloque proxy-test eliminado de $HOSTS"
    ;;
show)
    sed -n "/^$BEGIN\$/,/^$END\$/p" "$HOSTS"
    ;;
*)
    echo "uso: $0 add|remove|show" >&2
    exit 2
    ;;
esac
