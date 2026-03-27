cmake_minimum_required(VERSION 3.20)

project(mcqnet LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MCQNET_BUILD_BENCHMARKS "Build mcqnet benchmarks" OFF)
option(MCQNET_ENABLE_CLANG_TIDY "Run clang-tidy during C++ builds" OFF)

if(MCQNET_ENABLE_CLANG_TIDY)
    find_program(MCQNET_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${MCQNET_CLANG_TIDY_EXE}")
endif()

include(CTest)

add_subdirectory(mcqnet)
