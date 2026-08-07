#include "cc/parseit.hpp"

#include <gtest/gtest.h>

namespace {
struct value_grabber : cc::ast::visitor {
  std::int64_t v = -1;
  void visit(const cc::ast::program& p) override {
    for (const auto& s : p.body) s->accept(*this);
  }
  void visit(const cc::ast::return_stmt& r) override {
    if (r.value) r.value->accept(*this);
  }
  void visit(const cc::ast::int_literal& l) override { v = l.value; }
};
}  // namespace

TEST(cc_parseit, version) {
  EXPECT_EQ(cc::parseit::version(), "0.1.0");
}

TEST(cc_parseit, parse_return_int) {
  auto p = cc::parseit::parse("return 42;");
  ASSERT_TRUE(p.has_value());
  ASSERT_EQ(p->body.size(), 1u);
  value_grabber g;
  p->accept(g);
  EXPECT_EQ(g.v, 42);
}

TEST(cc_parseit, parse_whitespace_tolerated) {
  auto p = cc::parseit::parse("   return   7 ;  ");
  ASSERT_TRUE(p.has_value());
}

TEST(cc_parseit, parse_error_on_garbage) {
  auto p = cc::parseit::parse("not a program");
  EXPECT_FALSE(p.has_value());
}
