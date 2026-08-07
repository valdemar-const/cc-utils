#pragma once

#include "cc-astit_export.hpp"

#include <string_view>

namespace cc::astit {

[[nodiscard]] CC_ASTIT_API std::string_view version() noexcept;

}  // namespace cc::astit
