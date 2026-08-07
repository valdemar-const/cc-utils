#pragma once

#include "cc-parseit_export.hpp"

#include <cc/astit.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace cc::parseit {

[[nodiscard]] CC_PARSEIT_API std::string_view version() noexcept;

// Parse source text into a typed AST. Returns the parse error on failure.
[[nodiscard]] CC_PARSEIT_API std::expected<cc::ast::program, std::string>
parse(std::string_view source);

}  // namespace cc::parseit
