#include "cc/gen.hpp"

#include <gtest/gtest.h>

TEST(cc_gen, version) {
  EXPECT_EQ(cc::gen::version(), "0.1.0");
}

TEST(cc_gen, lower_ret_to_nasm_pod) {
  cc::ir::module m;
  m.code.push_back({cc::ir::opcode::ret, 42});

  std::vector<cc::nasm::instr> is = cc::gen::lower(m);
  ASSERT_EQ(is.size(), 3u);
  EXPECT_EQ(is[0].mn, cc::nasm::mnemonic::mov);  // mov rax, 60
  EXPECT_EQ(is[1].mn, cc::nasm::mnemonic::mov);  // mov rdi, 42
  EXPECT_EQ(is[2].mn, cc::nasm::mnemonic::syscall);
  EXPECT_EQ(is[1].ops[1].imm, 42);
}

TEST(cc_gen, format_listing) {
  cc::ir::module m;
  m.code.push_back({cc::ir::opcode::ret, 42});
  std::string s = cc::gen::format(cc::gen::lower(m));
  EXPECT_NE(s.find("_start"), std::string::npos);
  EXPECT_NE(s.find("mov rax"), std::string::npos);
  EXPECT_NE(s.find("syscall"), std::string::npos);
  EXPECT_NE(s.find("42"), std::string::npos);
}
