add_executable(cc-pipeit_test test_smoke.cpp)
target_link_libraries(cc-pipeit_test PRIVATE cc-pipeit::cc-pipeit GTest::gtest_main)
gtest_discover_tests(cc-pipeit_test)
