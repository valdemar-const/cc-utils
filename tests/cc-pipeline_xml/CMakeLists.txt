project(cc-pipeline_xml_test LANGUAGES CXX)

find_package(pugixml REQUIRED)

add_executable(cc-pipeline_xml_test
    test_pipeline_xml.cpp
    ${CMAKE_SOURCE_DIR}/projects/bin/cc-workbench/src/pipeline_xml.cpp
)
target_include_directories(cc-pipeline_xml_test PRIVATE
    ${CMAKE_SOURCE_DIR}/projects/bin/cc-workbench/src
)
target_link_libraries(cc-pipeline_xml_test PRIVATE
    cc-runtime::cc-runtime
    pugixml::pugixml
    GTest::gtest_main
)
gtest_discover_tests(cc-pipeline_xml_test
    PROPERTIES
        ENVIRONMENT "CCP_PLUGIN_PATH=${CMAKE_BINARY_DIR}/lib"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin"
)
