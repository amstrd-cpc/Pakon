# Unit tests. Each executable registers its own cases and is run by CTest.

add_executable(ppb_packet_test ppb/packet_test.cpp)
target_link_libraries(ppb_packet_test PRIVATE pakon_core)
target_include_directories(ppb_packet_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ppb_packet COMMAND ppb_packet_test)

add_executable(scanner_replay_test scanner/scanner_replay_test.cpp)
target_link_libraries(scanner_replay_test PRIVATE pakon_core)
target_include_directories(scanner_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scanner_replay COMMAND scanner_replay_test)

# Module-info decode: the capture-evidenced ASCII id window vs the lab
# unit's non-printable payloads (pure display logic, no hardware).
add_executable(scanner_module_info_test scanner/module_info_test.cpp)
target_link_libraries(scanner_module_info_test PRIVATE pakon_core)
target_include_directories(scanner_module_info_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scanner_module_info COMMAND scanner_module_info_test)

# Hardware-ID recognition + WinUSB INF consistency + WinUSB open-parameter
# pins (pure string/value logic, no hardware, no Windows APIs).
# PAKON_SOURCE_DIR lets the test read driver/PakonWinUSB.inf and pin its
# GUID to the C++ constant, and read both Windows open sites to pin them
# to usb/win_usb_open.hpp.
add_executable(usb_identity_test usb/identity_test.cpp)
target_link_libraries(usb_identity_test PRIVATE pakon_core)
target_include_directories(usb_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(usb_identity_test PRIVATE
    PAKON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME usb_identity COMMAND usb_identity_test)

# Bootstrap probe: pins the stage-1 personality read layout to the
# documented request and proves with a fake transport that a probe
# performs exactly one control read and no writes/bulk traffic (pure
# logic, no hardware, no Windows APIs).
add_executable(bootstrap_probe_test bootstrap/probe_test.cpp)
target_link_libraries(bootstrap_probe_test PRIVATE pakon_core)
target_include_directories(bootstrap_probe_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bootstrap_probe COMMAND bootstrap_probe_test)

# --- Scan path ------------------------------------------------------------

# Typed builders: every frame pinned to request hex recorded verbatim in
# the capture corpus; the per-mode constants against all six sessions.
add_executable(scan_commands_test scan/scan_commands_test.cpp)
target_link_libraries(scan_commands_test PRIVATE pakon_core)
target_include_directories(scan_commands_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scan_commands COMMAND scan_commands_test)

# Add the scan CLI test: it compiles apps/pakon-cli/scan_cli.cpp DIRECTLY
# (main.cpp stays out — no production USB opener is linked) and replaces
# the SessionOpener with a counting trap. That is the proof: default
# invocation and --dry-run exit without the opener ever being called, so
# no USB code could have run; --live-scan calls it exactly once.
add_executable(scan_cli_test cli/scan_cli_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/pakon-cli/scan_cli.cpp
    ${CMAKE_SOURCE_DIR}/apps/pakon-cli/interrupt.cpp)
target_link_libraries(scan_cli_test PRIVATE pakon_core)
target_include_directories(scan_cli_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/pakon-cli)
add_test(NAME scan_cli COMMAND scan_cli_test)

# Raw image output: PAKRAW01 header layout pinned byte-by-byte and the
# OEM-planar geometry, both read back from disk.
add_executable(raw_writer_test image/raw_writer_test.cpp)
target_link_libraries(raw_writer_test PRIVATE pakon_core)
target_include_directories(raw_writer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME raw_writer COMMAND raw_writer_test)

# --- Simulator-backed suites -------------------------------------------

# The fake F-135+ (tests/support/sim_device.*): PPB replies, controller
# state, EEPROM, and EP6 streaming on its own clock. Shared by every
# simulator-backed suite and by tools/sim_server (the Wine shim backend).
find_package(Threads REQUIRED)
add_library(pakon_sim STATIC support/sim_device.cpp)
target_link_libraries(pakon_sim PUBLIC pakon_core Threads::Threads)
target_include_directories(pakon_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

function(pakon_sim_test name source)
    add_executable(${name}_test ${source})
    target_link_libraries(${name}_test PRIVATE pakon_sim)
    add_test(NAME ${name} COMMAND ${name}_test)
endfunction()

pakon_sim_test(sim_device sim/sim_device_test.cpp)

# The simulator behind pkusb.dll's TCP protocol, for running the OEM stack
# under Wine against it (tools/wine/run_oem.sh). POSIX sockets: not built
# for Windows.
if(NOT WIN32)
    add_executable(pakon_sim_server support/sim_server.cpp)
    target_link_libraries(pakon_sim_server PRIVATE pakon_sim)
endif()
pakon_sim_test(image_stream stream/image_stream_test.cpp)
pakon_sim_test(eeprom eeprom/eeprom_test.cpp)

# Simulated full scans against a device that streams on its own clock:
# F1 (no line lost while commands run) and F2 (the host ends every
# window) — failed on the old runner (commit c3f9c3e, WILL_FAIL), pass on
# the rebuilt one; plus every configuration, first light, no film, late
# film, slow consumer, corrupt EEPROM, and a fault at every frame with
# the teardown sent exactly once.
pakon_sim_test(sim_scan scan/sim_scan_test.cpp)
set_tests_properties(sim_scan PROPERTIES TIMEOUT 900)

# Capture fidelity: the init block and the teardown order byte-compared
# with base4.jsonl; per-config geometry/integration/DX words against all
# six sessions.
pakon_sim_test(scan_capture scan/scan_capture_test.cpp)

# Session tools (tools/pcap_to_jsonl.py, tools/compare_sessions.py) on
# synthetic USBPcap captures; skipped when no Python 3 interpreter exists.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME session_tools
             COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/tests/test_session_tools.py)
endif()
