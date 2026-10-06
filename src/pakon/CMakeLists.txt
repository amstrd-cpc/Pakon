# Core library: pakon_core
#
# Layering (top depends on bottom):
#   scanner → ppb → usb
#   all layers use errors/ and logging/
# protocol/ is header-only constants, documented against pakon-reference.

set(PAKON_CORE_SOURCES
    errors/error.cpp
    logging/logger.cpp
    usb/transport.cpp
    ppb/packet.cpp
    ppb/client.cpp
)

if(WIN32)
    list(APPEND PAKON_CORE_SOURCES
        usb/enumerate_win.cpp
        usb/win_usb_transport.cpp
    )
else()
    # Non-Windows stub: enumeration returns empty, open_first reports
    # usb_not_supported. Transport-independent layers still build and test.
    list(APPEND PAKON_CORE_SOURCES
        usb/transport_stub.cpp
    )
endif()

add_library(pakon_core STATIC ${PAKON_CORE_SOURCES})

target_include_directories(pakon_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
