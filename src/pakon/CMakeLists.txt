# Core library: pakon_core
#
# Layering (top depends on bottom):
#   scanner → ppb → usb
#   all layers use errors/ and logging/
# protocol/ is header-only constants, documented against pakon-reference.

set(PAKON_CORE_SOURCES
    errors/error.cpp
    logging/logger.cpp
)

add_library(pakon_core STATIC ${PAKON_CORE_SOURCES})

target_include_directories(pakon_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
