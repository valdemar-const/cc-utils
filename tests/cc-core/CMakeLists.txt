add_executable(cc-core_test test_smoke.cpp)
target_link_libraries(cc-core_test PRIVATE cc-core::cc-core GTest::gtest_main)
gtest_discover_tests(cc-core_test)
