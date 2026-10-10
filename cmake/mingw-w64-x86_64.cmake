# Cross-compile pakon-cli.exe and the test executables for Windows x64
# with MinGW-w64:
#   cmake -S . -B build-mingw -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
# Set MINGW_PREFIX if the compilers are not on PATH as
# x86_64-w64-mingw32-g++.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_triple x86_64-w64-mingw32)
if(DEFINED ENV{MINGW_PREFIX})
    set(_prefix "$ENV{MINGW_PREFIX}/")
endif()
set(CMAKE_C_COMPILER ${_prefix}${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_prefix}${_triple}-g++)
set(CMAKE_RC_COMPILER ${_prefix}${_triple}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Self-contained executables: no libstdc++/libgcc/winpthread DLLs to ship.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
