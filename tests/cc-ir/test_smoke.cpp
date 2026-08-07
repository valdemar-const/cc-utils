#include "cc/ir.hpp"

#include <gtest/gtest.h>

TEST(cc_ir, version) {
  EXPECT_EQ(cc::ir::version(), "0.1.0");
}

TEST(cc_ir, module_holds_instructions) {
  cc::ir::module m;
  m.code.push_back({cc::ir::opcode::ret, 42});
  ASSERT_EQ(m.code.size(), 1u);
  EXPECT_EQ(m.code[0].op, cc::ir::opcode::ret);
  EXPECT_EQ(m.code[0].imm, 42);
}
