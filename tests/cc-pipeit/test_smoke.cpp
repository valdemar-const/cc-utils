#include "cc/pipeit.hpp"

#include <gtest/gtest.h>

TEST(cc_pipeit, version) {
  EXPECT_EQ(cc::pipeit::version(), "0.1.0");
}
