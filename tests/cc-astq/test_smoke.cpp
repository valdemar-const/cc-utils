#include "cc/astq.hpp"

#include <gtest/gtest.h>
#include <memory>

TEST(cc_astq, version) {
  EXPECT_EQ(cc::astq::version(), "0.1.0");
}

TEST(cc_astq, lowers_return_to_one_ir_instr) {
  using namespace cc::ast;
  auto lit = std::make_unique<int_literal>();
  lit->value = 42;
  auto ret = std::make_unique<return_stmt>();
  ret->value = std::move(lit);
  program prog;
  prog.body.push_back(std::move(ret));

  cc::ir::module m = cc::astq::lower(prog);
  ASSERT_EQ(m.code.size(), 1u);
  EXPECT_EQ(m.code[0].op, cc::ir::opcode::ret);
  EXPECT_EQ(m.code[0].imm, 42);
}

TEST(cc_astq, empty_program_yields_empty_ir) {
  cc::ast::program prog;
  cc::ir::module m = cc::astq::lower(prog);
  EXPECT_TRUE(m.code.empty());
}
