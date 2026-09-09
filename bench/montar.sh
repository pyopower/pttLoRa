#!/bin/bash
# montar.sh — junta las muestras en un solo wav con locuciones, para oirlo
# del tirón en el movil sin ir fichero a fichero.
set -e
S=salida; T=$S/.tmp; O=${1:-comparativa-codecs.wav}
di() { espeak -v es -s 145 -w "$T/l.wav" "$1" 2>/dev/null
       sox "$T/l.wav" -r 8000 -c 1 -b 16 "$T/lab$2.wav" gain -3; }
i=0; lista=()
add() { i=$((i+1)); di "$1" $i; lista+=("$T/lab$i.wav" "$2" "$T/sil.wav"); }
sox -n -r 8000 -c 1 -b 16 "$T/sil.wav" trim 0 0.7
add "Original, sin comprimir"                       "$S/00-original.wav"
add "Codec dos, tres mil doscientos"                "$S/01-codec2-3200.wav"
add "Mil seiscientos"                               "$S/01-codec2-1600.wav"
add "Mil doscientos"                                "$S/01-codec2-1200.wav"
add "Setecientos ce"                                "$S/01-codec2-700C.wav"
add "Cuatrocientos cincuenta"                       "$S/01-codec2-450.wav"
add "Ahora con el veinticinco por ciento de paquetes perdidos. Primero, mil doscientos enviado una vez" "$S/02-perdida25-1200-simple.wav"
add "Y ahora setecientos ce enviado dos veces, con el mismo gasto de aire y el mismo canal" "$S/02-perdida25-700C-doble.wav"
sox "${lista[@]}" "$O"
echo "-> $O  ($(soxi -d "$O"))"
