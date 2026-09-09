#!/usr/bin/env python3
"""Parche a codec2 para poder cruzar-compilarlo con el NDK de Android.

Al cruzar-compilar, codec2 lanza un sub-proyecto (`codec2_native`) que compila
para el HOST la herramienta que genera sus libros de codigo. Ese sub-proyecto
hereda el generador del padre — Ninja, puesto por el toolchain de Android —
pero no un compilador de host, y muere con:

    CMake Error: CMake was unable to find a build program corresponding to
    "Ninja".  CMAKE_MAKE_PROGRAM is not set.
    CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage

Se le dice explicitamente que use Makefiles y el `cc` del sistema. No se toca
nada mas de codec2, y el parche es idempotente.

Uso: parche_codec2.py <ruta a codec2/src/CMakeLists.txt>
"""
import sys

MARCA = "PARCHE-PTTLORA"

VIEJO = """    ExternalProject_Add(codec2_native
       SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..
       BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/codec2_native"""

NUEVO = """    # PARCHE-PTTLORA: el sub-proyecto nativo heredaba el generador de
    # Android y se quedaba sin compilador de host.
    ExternalProject_Add(codec2_native
       SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..
       BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/codec2_native
       CMAKE_GENERATOR "Unix Makefiles"
       CMAKE_ARGS -DCMAKE_C_COMPILER=cc -DUNITTEST=OFF -DBUILD_SHARED_LIBS=OFF"""


def main():
    ruta = sys.argv[1]
    s = open(ruta).read()
    if MARCA in s:
        print("codec2 ya estaba parcheado")
        return 0
    if VIEJO not in s:
        print("AVISO: el bloque codec2_native ha cambiado en codec2; "
              "revisar parche_codec2.py contra la version nueva")
        return 1
    open(ruta, "w").write(s.replace(VIEJO, NUEVO, 1))
    print("codec2 parcheado")
    return 0


if __name__ == "__main__":
    sys.exit(main())
