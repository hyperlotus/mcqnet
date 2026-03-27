file(GLOB MCQNET_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)

foreach(test_source IN LISTS MCQNET_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)

    add_executable("${test_name}"
        "${test_source}"
    )

    target_link_libraries("${test_name}"
        PRIVATE
            mcqnet::mcqnet
    )

    add_test(
        NAME "${test_name}"
        COMMAND "${test_name}"
    )
endforeach()
