#!/bin/bash
# Trae Codec2 para los firmwares con audio propio (`*-audio`). Reutiliza el
# banco de bench/codec2-esp32, que es quien sabe generar los libros de codigos:
# ahi se compila nativo una vez y los .c generados valen para cualquier CPU.
set -e
AQUI=$(cd "$(dirname "$0")" && pwd)
BANCO="$AQUI/../bench/codec2-esp32"
[ -f "$BANCO/lib/codec2/codec2.c" ] || "$BANCO/preparar.sh"
mkdir -p "$AQUI/lib-audio"
rm -rf "$AQUI/lib-audio/codec2"
cp -r "$BANCO/lib/codec2" "$AQUI/lib-audio/codec2"
echo "listo — ahora: pio run -e lora32-audio"
