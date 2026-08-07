#include "cc/gen.hpp"

#include <gtest/gtest.h>

TEST(cc_gen, version) {
  EXPECT_EQ(cc::gen::version(), "0.1.0");
}
