#include "cc/astq.hpp"

#include <gtest/gtest.h>

TEST(cc_astq, version) {
  EXPECT_EQ(cc::astq::version(), "0.1.0");
}
