#pragma once

#include "cc-astq_export.hpp"

#include <cc/astit.hpp>
#include <cc/ir.hpp>

namespace cc::astq {

[[nodiscard]] CC_ASTQ_API std::string_view version() noexcept;

// Traverse the AST node-by-node (visitor) and emit IR instructions. Each
// relevant node lowers to one ir::instr; e.g. return_stmt -> {ret, <imm>}.
[[nodiscard]] CC_ASTQ_API cc::ir::module lower(const cc::ast::program& root);

}  // namespace cc::astq
