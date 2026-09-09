cmake_minimum_required(VERSION 3.22)
project(pttlora C)

# codec2 se compila desde fuente, igual que se hizo con libopus en la app PTT.
# El arbol vive SOLO en la maquina de compilacion (no en el repo): lo clona
# `preparar.sh`.
#
# TRAMPA: codec2 genera sus libros de codigo con `generate_codebook`, que se
# EJECUTA durante el build. Al cruzar-compilar para Android hay que darle un
# binario ya compilado para el host, o el build intenta ejecutar un ARM en x86.
set(UNITTEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(codec2)

add_library(pttlora SHARED codec2_jni.c)
target_include_directories(pttlora PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src
    ${CMAKE_CURRENT_BINARY_DIR}/codec2)
target_link_libraries(pttlora codec2 log)
