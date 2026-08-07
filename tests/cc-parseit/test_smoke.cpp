#include "cc/parseit.hpp"

#include <gtest/gtest.h>

TEST(cc_parseit, version) {
  EXPECT_EQ(cc::parseit::version(), "0.1.0");
}
