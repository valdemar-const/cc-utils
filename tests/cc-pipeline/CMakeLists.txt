add_executable(cc-pipeline_test test_pipeline.cpp)
target_link_libraries(cc-pipeline_test PRIVATE
    cc-runtime::cc-runtime
    GTest::gtest_main
)
gtest_discover_tests(cc-pipeline_test
    PROPERTIES
        ENVIRONMENT "CCP_PLUGIN_PATH=${CMAKE_BINARY_DIR}/lib"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin"
)
