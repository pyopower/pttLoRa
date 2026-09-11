/* secreto-ejemplo.h — PLANTILLA. Copiala a `secreto.h` y pon el tuyo.
 *
 *     cp src/secreto-ejemplo.h src/secreto.h
 *     python3 -c "import secrets;print(secrets.token_hex(16))"   # y pegalo
 *
 * `secreto.h` esta en el `.gitignore` y NO se sube: es lo unico que separa tu
 * relevo de mando de cualquiera que clone el repositorio. Ofuscarlo dentro del
 * codigo publicado no habria servido de nada —se lee clonando, y un `strings`
 * sobre el binario tambien lo saca—, asi que directamente no esta.
 *
 * El mismo texto va en el servidor, en el fichero que se le pasa al relevo con
 * `--secreto` (ver `tools/mandovirtual.py`).
 *
 * Si este fichero no existe, el firmware compila igual: el nodo se queda sin
 * secreto, no contesta a los retos y lo dice en su estado (`secreto=no`). Es a
 * proposito: quien clone el proyecto tiene que poder compilarlo sin tener que
 * inventarse nada.
 *
 * ⚠️ Cambiar el secreto obliga a reflashear TODOS los nodos que usen el canal
 * de mando. Piensalo antes de rotarlo por gusto.
 */
#pragma once

#define MANDO_SECRETO "pon-aqui-el-tuyo-y-no-lo-subas"
