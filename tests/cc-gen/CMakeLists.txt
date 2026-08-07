add_executable(cc-gen_test test_smoke.cpp)
target_link_libraries(cc-gen_test PRIVATE cc-gen::cc-gen GTest::gtest_main)
gtest_discover_tests(cc-gen_test)
