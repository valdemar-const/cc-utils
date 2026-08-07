add_executable(cc-parseit_test test_smoke.cpp)
target_link_libraries(cc-parseit_test PRIVATE cc-parseit::cc-parseit GTest::gtest_main)
gtest_discover_tests(cc-parseit_test)
