#include "cc/gen.hpp"

#include <gtest/gtest.h>

TEST(cc_gen, version) {
  EXPECT_EQ(cc::gen::version(), "0.1.0");
}

TEST(cc_gen, lower_ret_to_nasm_pod) {
  cc::ir::module m;
  m.code.push_back({cc::ir::opcode::ret, 42});

  std::vector<cc::nasm::instr> is = cc::gen::lower(m);
#if defined(_WIN32)
  // Win32/PE: `main` returns imm in rax, then ret; the gcc-pulled CRT calls
  // ExitProcess with main's return value (no raw exit syscall on Windows).
  ASSERT_EQ(is.size(), 2u);
  EXPECT_EQ(is[0].mn, cc::nasm::mnemonic::mov);   // mov rax, 42
  EXPECT_EQ(is[1].mn, cc::nasm::mnemonic::ret);
  EXPECT_EQ(is[0].ops[1].imm, 42);
#else
  // Linux/ELF: sys_exit(imm) via the raw x86_64 syscall interface.
  ASSERT_EQ(is.size(), 3u);
  EXPECT_EQ(is[0].mn, cc::nasm::mnemonic::mov);  // mov rax, 60
  EXPECT_EQ(is[1].mn, cc::nasm::mnemonic::mov);  // mov rdi, 42
  EXPECT_EQ(is[2].mn, cc::nasm::mnemonic::syscall);
  EXPECT_EQ(is[1].ops[1].imm, 42);
#endif
}

TEST(cc_gen, format_listing) {
  cc::ir::module m;
  m.code.push_back({cc::ir::opcode::ret, 42});
  std::string s = cc::gen::format(cc::gen::lower(m));
#if defined(_WIN32)
  EXPECT_NE(s.find("main"), std::string::npos);
  EXPECT_NE(s.find("mov rax"), std::string::npos);
  EXPECT_NE(s.find("ret"), std::string::npos);
#else
  EXPECT_NE(s.find("_start"), std::string::npos);
  EXPECT_NE(s.find("mov rax"), std::string::npos);
  EXPECT_NE(s.find("syscall"), std::string::npos);
#endif
  EXPECT_NE(s.find("42"), std::string::npos);
}
