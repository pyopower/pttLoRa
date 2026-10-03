#!/bin/bash
# Deja el banco listo para compilar. El arbol de codec2 NO va en el repo: son
# 100 MB de clon para sacar 24 ficheros.
#
# Hay un paso que no es obvio: codec2 GENERA sus libros de codigos durante el
# build (`generate_codebook` se ejecuta en la maquina). Por eso primero se
# compila nativo aqui y despues se copian los .c generados, que no dependen de
# la arquitectura.
set -e
AQUI=$(cd "$(dirname "$0")" && pwd)
cd "$AQUI"

[ -d codec2-fuente ] || git clone --depth 1 https://github.com/drowe67/codec2.git codec2-fuente
mkdir -p codec2-fuente/build
(cd codec2-fuente/build && cmake .. -DUNITTEST=OFF -DBUILD_SHARED_LIBS=OFF \
     -DINSTALL_EXAMPLES=OFF >/dev/null && make -j4 codec2 >/dev/null)

mkdir -p lib/codec2/codec2
FUENTES="codec2.c lpc.c nlp.c postfilter.c sine.c codec2_fft.c kiss_fft.c \
         kiss_fftr.c interp.c lsp.c phase.c quantise.c pack.c dump.c \
         newamp1.c mbest.c"
for f in $FUENTES; do cp "codec2-fuente/src/$f" lib/codec2/; done
cp codec2-fuente/src/*.h lib/codec2/
cp codec2-fuente/build/src/codebook.c codec2-fuente/build/src/codebookd.c \
   codec2-fuente/build/src/codebookge.c codec2-fuente/build/src/codebookjmv.c \
   codec2-fuente/build/src/codebooknewamp1.c \
   codec2-fuente/build/src/codebooknewamp1_energy.c lib/codec2/
cp codec2-fuente/build/config.h lib/codec2/
cp codec2-fuente/build/codec2/version.h lib/codec2/codec2/

cat > lib/codec2/library.json <<'JSON'
{
  "name": "codec2",
  "version": "1.2.0",
  "build": { "flags": ["-I."], "srcFilter": ["+<*.c>"] }
}
JSON
echo "listo — ahora: pio run -e banco -t upload"
