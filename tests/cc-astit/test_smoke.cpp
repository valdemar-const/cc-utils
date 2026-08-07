#include "cc/astit.hpp"

#include <gtest/gtest.h>

TEST(cc_astit, version) {
  EXPECT_EQ(cc::astit::version(), "0.1.0");
}
