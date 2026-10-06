# Unit tests. Each executable registers its own cases and is run by CTest.

add_executable(ppb_packet_test ppb/packet_test.cpp)
target_link_libraries(ppb_packet_test PRIVATE pakon_core)
target_include_directories(ppb_packet_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ppb_packet COMMAND ppb_packet_test)

add_executable(scanner_replay_test scanner/scanner_replay_test.cpp)
target_link_libraries(scanner_replay_test PRIVATE pakon_core)
target_include_directories(scanner_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scanner_replay COMMAND scanner_replay_test)
