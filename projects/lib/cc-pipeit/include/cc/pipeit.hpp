#pragma once

#include "cc-pipeit_export.hpp"

#include <string_view>

namespace cc::pipeit {

[[nodiscard]] CC_PIPEIT_API std::string_view version() noexcept;

}  // namespace cc::pipeit
