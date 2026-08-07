#pragma once

#include "cc-gen_export.hpp"

#include <string_view>

namespace cc::gen {

[[nodiscard]] CC_GEN_API std::string_view version() noexcept;

}  // namespace cc::gen
