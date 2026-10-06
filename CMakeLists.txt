cmake_minimum_required(VERSION 3.20)

project(pakon
    VERSION 0.1.0
    DESCRIPTION "Native C++ driver stack for the Kodak/Pakon F-X35 film scanners"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PAKON_BUILD_TESTS "Build unit tests" ON)
option(PAKON_BUILD_APPS "Build command-line applications" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(src/pakon)

if(PAKON_BUILD_APPS)
    add_subdirectory(apps/pakon-cli)
endif()

if(PAKON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
