add_executable(pakon-cli main.cpp scan_cli.cpp frames_cli.cpp interrupt.cpp)
target_link_libraries(pakon-cli PRIVATE pakon_core pakon_process)
