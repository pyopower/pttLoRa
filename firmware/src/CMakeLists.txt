# Solo para el entorno `transceptor` (Arduino como componente de ESP-IDF); los
# demas entornos son Arduino a secas y no leen esto.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources})

# ESP-IDF compila con -Werror y el codigo de siempre trae avisos inofensivos en
# el ESP32: `%lu` con uint32_t (los dos son de 32 bits), comentarios `//` que
# acaban en barra y snprintf que podrian recortar. Se dejan como AVISOS: se
# ven, pero no paran la compilacion. Tiene que ir aqui y no en build_flags:
# las opciones del componente van detras y mandan.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=comment)
