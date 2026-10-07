# Unit tests. Each executable registers its own cases and is run by CTest.

add_executable(ppb_packet_test ppb/packet_test.cpp)
target_link_libraries(ppb_packet_test PRIVATE pakon_core)
target_include_directories(ppb_packet_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ppb_packet COMMAND ppb_packet_test)

add_executable(scanner_replay_test scanner/scanner_replay_test.cpp)
target_link_libraries(scanner_replay_test PRIVATE pakon_core)
target_include_directories(scanner_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scanner_replay COMMAND scanner_replay_test)

# Hardware-ID recognition + WinUSB INF consistency (pure string logic,
# no hardware, no Windows APIs). PAKON_SOURCE_DIR lets the test read
# driver/PakonWinUSB.inf and pin its GUID to the C++ constant.
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
