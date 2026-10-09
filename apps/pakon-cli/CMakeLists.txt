add_executable(pakon-cli main.cpp scan_cli.cpp)
target_link_libraries(pakon-cli PRIVATE pakon_core)
