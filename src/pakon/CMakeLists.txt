# Core library: pakon_core
#
# Layering (top depends on bottom):
#   scanner → ppb → usb
#   scan → image, protocol, ppb  (session plans + image reception; no I/O
#   in session/plan, interfaces only in transport/runner)
#   bootstrap → usb   (cold-device read-only probe; no PPB involvement)
#   all layers use errors/ and logging/
# protocol/ is header-only constants, documented against pakon-reference.

set(PAKON_CORE_SOURCES
    errors/error.cpp
    logging/logger.cpp
    usb/transport.cpp
    bootstrap/probe.cpp
    ppb/packet.cpp
    ppb/client.cpp
    scanner/scanner.cpp
    scan/session.cpp
    scan/plan.cpp
    scan/runner.cpp
    image/row_sync.cpp
    image/reader.cpp
    image/raw_writer.cpp
    stream/image_stream.cpp
    eeprom/eeprom.cpp
)

if(WIN32)
    list(APPEND PAKON_CORE_SOURCES
        usb/enumerate_win.cpp
        usb/win_usb_transport.cpp
        usb/driver_attrib_win.cpp
        usb/descriptors_win.cpp
    )
else()
    # Non-Windows stub: enumeration returns empty, open_first reports
    # usb_not_supported, driver attribution and descriptor discovery
    # report unsupported. Transport-independent layers still build and test.
    list(APPEND PAKON_CORE_SOURCES
        usb/transport_stub.cpp
        usb/driver_attrib_stub.cpp
        usb/descriptors_stub.cpp
    )
endif()

add_library(pakon_core STATIC ${PAKON_CORE_SOURCES})

target_include_directories(pakon_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32)
    # Windows system import libraries used by usb/enumerate_win.cpp
    # (SetupAPI enumeration) and usb/win_usb_transport.cpp (WinUSB pipe
    # I/O). PRIVATE keeps them off the compile interface; for a static
    # library CMake still propagates them to consumers as
    # $<LINK_ONLY:...>, so executables linking pakon_core resolve them
    # at link time. No SDK paths are hardcoded - the MSVC toolchain
    # resolves these by name.
    target_link_libraries(pakon_core PRIVATE
        setupapi
        winusb
        ole32
    )

    # GUID_DEVINTERFACE_USB_DEVICE is declared by usbiodef.h but defined
    # by no SDK library (verified by scanning all um/x64 import libs);
    # the header documentation requires "include initguid.h first, then
    # include usbiodef.h" so this translation unit emits the definition.
    # The forced include must precede everything else because guiddef.h
    # latches the INITGUID-dependent DEFINE_GUID form at first inclusion
    # (its defining form is DECLSPEC_SELECTANY, so no duplicate-symbol
    # risk). Source files left unmodified; GNU toolchains resolve the
    # GUID from their import libraries and need no forced include.
    set_source_files_properties(usb/enumerate_win.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC,Clang>:/FIinitguid.h>"
    )
endif()
