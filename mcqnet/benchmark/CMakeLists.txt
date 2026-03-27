file(GLOB MCQNET_BENCHMARK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

foreach(benchmark_source IN LISTS MCQNET_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)

    add_executable("${benchmark_name}"
        "${benchmark_source}"
    )

    target_link_libraries("${benchmark_name}"
        PRIVATE
            mcqnet::mcqnet
    )
endforeach()
