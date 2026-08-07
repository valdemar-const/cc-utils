#include "cc/ir.hpp"

#include <gtest/gtest.h>

TEST(cc_ir, version) {
  EXPECT_EQ(cc::ir::version(), "0.1.0");
}
