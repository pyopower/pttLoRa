#!/bin/bash
# compruebasecreto.sh — se niega a dejar pasar un binario que lleve dentro el
# secreto del canal de mando.
#
#   tools/compruebasecreto.sh firmware/.pio/build/lora32-publico/firmware.bin
#
# Pasalo SIEMPRE antes de copiar un .bin al arbol publico o a descargas: ahi
# hay un flasher web, y un binario con el secreto publicado es el secreto
# publicado. Un `strings` lo saca en un segundo, que es justo lo que hace esto.
set -u
SEC_H="$(dirname "$0")/../firmware/src/secreto.h"
[ -f "$SEC_H" ] || { echo "no hay secreto.h: nada que comprobar"; exit 0; }
SEC=$(grep -oP '(?<=#define MANDO_SECRETO ")[^"]+' "$SEC_H") || exit 1
[ -n "$SEC" ] || { echo "secreto.h sin valor"; exit 1; }
[ $# -gt 0 ] || { echo "uso: $0 <fichero.bin> [...]"; exit 1; }
mal=0
for f in "$@"; do
    # PRIMERO que el fichero este y se pueda leer. Sin esto, un `strings` sobre
    # un fichero que no existe no encuentra el secreto y la comprobacion dice
    # OK: aprobar lo que no se ha podido mirar es peor que no comprobar nada.
    # Paso de verdad el 11-sep, con un .bin que PlatformIO acababa de limpiar.
    if [ ! -r "$f" ] || [ ! -s "$f" ]; then
        echo "⛔ $f NO SE PUEDE LEER (o esta vacio) — no se ha comprobado nada"
        mal=1
        continue
    fi
    if strings -n 8 "$f" 2>/dev/null | grep -qF "$SEC"; then
        echo "⛔ $f LLEVA EL SECRETO DENTRO — no lo publiques"
        mal=1
    else
        echo "OK $f"
    fi
done
exit $mal
