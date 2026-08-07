add_executable(cc-astq_test test_smoke.cpp)
target_link_libraries(cc-astq_test PRIVATE cc-astq::cc-astq GTest::gtest_main)
gtest_discover_tests(cc-astq_test)
