add_executable(cc-astit_test test_smoke.cpp)
target_link_libraries(cc-astit_test PRIVATE cc-astit::cc-astit GTest::gtest_main)
gtest_discover_tests(cc-astit_test)
