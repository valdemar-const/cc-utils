#pragma once

#include "cc-astit_export.hpp"

#include <cc/any_ast.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace cc::astit {
[[nodiscard]] CC_ASTIT_API std::string_view version() noexcept;
}  // namespace cc::astit

namespace cc::ast {

struct visitor;

// Abstract base of the concrete AST. All node classes are polymorphic AND used
// across the DSO boundary (built in the frontend plugin, traversed in the
// ir_generator plugin via cc-astq), so they carry default visibility + an
// out-of-line key function (accept/dtor) to anchor a single shared vtable in
// libcc-astit.
struct CC_ASTIT_API node {
  virtual ~node();
  virtual void accept(visitor& v) const = 0;
};

struct CC_ASTIT_API int_literal : node {
  std::int64_t value = 0;
  void accept(visitor& v) const override;
};

struct CC_ASTIT_API return_stmt : node {
  std::unique_ptr<node> value;  // owned; points at the expression
  void accept(visitor& v) const override;
};

struct CC_ASTIT_API program : node {
  std::vector<std::unique_ptr<node>> body;  // top-level statements, owned
  void accept(visitor& v) const override;
};

// Classic double-dispatch visitor. Override only the nodes you care about.
struct CC_ASTIT_API visitor {
  virtual ~visitor();
  virtual void visit(const program&) {}
  virtual void visit(const return_stmt&) {}
  virtual void visit(const int_literal&) {}
};

// Concrete carrier for "tl"-language ASTs. The tl frontend wraps its parsed
// cc::ast::program here; the tl ir_generator downcasts back. Lives in cc-astit
// so both tl plugins share tl_program's typeinfo for the cross-DSO dynamic_cast.
struct CC_ASTIT_API tl_program : cc::IAnyAst {
  std::unique_ptr<program> root;
  ~tl_program() override;
};

}  // namespace cc::ast
