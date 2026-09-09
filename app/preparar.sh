#!/bin/bash
# preparar.sh — deja el arbol de codec2 listo para compilar la app.
# Se ejecuta UNA VEZ en la maquina de compilacion (el dl360).
#
# codec2 no va en el repo: son decenas de MB y solo hace falta aqui, igual que
# se hizo con libopus en la app PTT.
set -e
AQUI=$(cd "$(dirname "$0")" && pwd)
CPP="$AQUI/app/src/main/cpp"

[ -d "$CPP/codec2" ] || git clone --depth 1 https://github.com/drowe67/codec2.git "$CPP/codec2"

# Ver parche_codec2.py: sin esto el cruce con el NDK no configura.
python3 "$AQUI/parche_codec2.py" "$CPP/codec2/src/CMakeLists.txt"

echo "listo"
