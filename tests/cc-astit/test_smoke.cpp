#include "cc/astit.hpp"

#include <gtest/gtest.h>
#include <memory>

TEST(cc_astit, version) {
  EXPECT_EQ(cc::astit::version(), "0.1.0");
}

TEST(cc_astit, build_and_traverse) {
  using namespace cc::ast;
  auto lit = std::make_unique<int_literal>();
  lit->value = 7;
  auto ret = std::make_unique<return_stmt>();
  ret->value = std::move(lit);
  program prog;
  prog.body.push_back(std::move(ret));

  struct counter : visitor {
    int programs = 0, returns = 0, lits = 0;
    void visit(const program& p) override {
      ++programs;
      for (const auto& s : p.body) s->accept(*this);
    }
    void visit(const return_stmt& r) override {
      ++returns;
      if (r.value) r.value->accept(*this);
    }
    void visit(const int_literal&) override { ++lits; }
  } v;
  prog.accept(v);
  EXPECT_EQ(v.programs, 1);
  EXPECT_EQ(v.returns, 1);
  EXPECT_EQ(v.lits, 1);
}

TEST(cc_astit, tl_program_downcasts_from_anyast) {
  auto carrier = std::make_unique<cc::ast::tl_program>();
  carrier->root = std::make_unique<cc::ast::program>();
  cc::IAnyAst* erased = carrier.get();
  auto* back = dynamic_cast<cc::ast::tl_program*>(erased);
  EXPECT_NE(back, nullptr);
}
