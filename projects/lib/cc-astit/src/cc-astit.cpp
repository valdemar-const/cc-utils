#include "cc/astit.hpp"

namespace cc::astit {

std::string_view version() noexcept {
  return "0.1.0";
}

}  // namespace cc::astit

namespace cc::ast {

// Out-of-line key functions: one shared vtable per class, owned by libcc-astit.
node::~node() = default;
visitor::~visitor() = default;
tl_program::~tl_program() = default;

void int_literal::accept(visitor& v) const { v.visit(*this); }
void return_stmt::accept(visitor& v) const { v.visit(*this); }
void program::accept(visitor& v) const { v.visit(*this); }

}  // namespace cc::ast
