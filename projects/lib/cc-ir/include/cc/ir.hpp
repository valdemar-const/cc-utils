#pragma once

#include "cc-ir_export.hpp"

#include <string_view>

namespace cc::ir {

[[nodiscard]] CC_IR_API std::string_view version() noexcept;

// Minimal IR unit crossing the plugin boundary. Will grow real ops/SSA later.
struct module {
  int exit_code = 0;
};

}  // namespace cc::ir
