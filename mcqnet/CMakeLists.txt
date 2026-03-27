cmake_minimum_required(VERSION 3.20)

add_library(mcqnet INTERFACE)
add_library(mcqnet::mcqnet ALIAS mcqnet)

target_compile_features(mcqnet
    INTERFACE
        cxx_std_20
)

target_include_directories(mcqnet
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Provide a real translation unit for clangd when a tooling entrypoint exists.
set(MCQNET_CLANGD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tooling/mcqnet.cc")
if(EXISTS "${MCQNET_CLANGD_SOURCE}")
    add_executable(mcqnet_clangd
        "${MCQNET_CLANGD_SOURCE}"
    )

    target_link_libraries(mcqnet_clangd
        PRIVATE
            mcqnet::mcqnet
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

if(MCQNET_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
