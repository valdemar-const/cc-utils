#include "cc/any_value.hpp"
#include "cc/node.hpp"
#include "cc/registry.hpp"
#include "cc/view.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

struct point { int x, y; };

}  // namespace

// ---- any_value mechanics ----------------------------------------------------

TEST(cc_core_any_value, holds_string_and_round_trips) {
  cc::any_value v = std::string("hello");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v.type_descriptor(), cc::descriptor_of<std::string>);

  auto* p = aa::any_cast<std::string>(&v);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, "hello");
}

TEST(cc_core_any_value, holds_custom_type) {
  cc::any_value v = point{3, 4};
  EXPECT_EQ(v.type_descriptor(), cc::descriptor_of<point>);

  auto* p = aa::any_cast<point>(&v);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->x, 3);
  EXPECT_EQ(p->y, 4);
}

TEST(cc_core_any_value, copy_preserves_value_and_type) {
  cc::any_value a = std::string("abc");
  cc::any_value b = a;  // copy
  EXPECT_TRUE(b.has_value());
  EXPECT_EQ(b.type_descriptor(), cc::descriptor_of<std::string>);
  EXPECT_EQ(*aa::any_cast<std::string>(&b), "abc");
}

TEST(cc_core_any_value, move_preserves_value_and_type) {
  cc::any_value a = std::string("abc");
  cc::any_value c = std::move(a);
  EXPECT_TRUE(c.has_value());
  EXPECT_EQ(c.type_descriptor(), cc::descriptor_of<std::string>);
  EXPECT_EQ(*aa::any_cast<std::string>(&c), "abc");
}

TEST(cc_core_any_value, wrong_type_cast_returns_null) {
  cc::any_value v = 42;
  EXPECT_EQ(aa::any_cast<std::string>(&v), nullptr);
}

TEST(cc_core_any_value, default_constructed_is_empty) {
  cc::any_value v;
  EXPECT_FALSE(v.has_value());
}

TEST(cc_core_any_value, reassign_changes_type) {
  cc::any_value v = std::string("a");
  v = point{1, 2};
  EXPECT_EQ(v.type_descriptor(), cc::descriptor_of<point>);
  EXPECT_NE(v.type_descriptor(), cc::descriptor_of<std::string>);
}

// ---- descriptor_t behaviour -------------------------------------------------

TEST(cc_core_descriptor, equal_for_same_type) {
  EXPECT_EQ(cc::descriptor_of<int>, cc::descriptor_of<int>);
  EXPECT_EQ(cc::descriptor_of<std::string>, cc::descriptor_of<std::string>);
}

TEST(cc_core_descriptor, differs_for_different_types) {
  EXPECT_NE(cc::descriptor_of<int>, cc::descriptor_of<std::string>);
  EXPECT_NE(cc::descriptor_of<point>, cc::descriptor_of<int>);
}

// ---- compile-time contract check -------------------------------------------

// Touch the abstract interfaces to ensure they are usable as anchors.
// (Just compilation of this TU links against the vtables in libcc-core.)
TEST(cc_core_interfaces, abstract_bases_compile) {
  // Sanity: enums are usable.
  cc::slot_dir d = cc::slot_dir::in;
  cc::slot_card c = cc::slot_card::single;
  EXPECT_EQ(static_cast<int>(d), static_cast<int>(cc::slot_dir::in));
  EXPECT_EQ(static_cast<int>(c), static_cast<int>(cc::slot_card::single));

  // failure is aggregate-constructible.
  cc::failure f{"boom"};
  EXPECT_EQ(f.what, "boom");
}
