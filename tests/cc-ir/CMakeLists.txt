add_executable(cc-ir_test test_smoke.cpp)
target_link_libraries(cc-ir_test PRIVATE cc-ir::cc-ir GTest::gtest_main)
gtest_discover_tests(cc-ir_test)
