#pragma once

#include "cc-parseit_export.hpp"

#include <string_view>

namespace cc::parseit {

[[nodiscard]] CC_PARSEIT_API std::string_view version() noexcept;

}  // namespace cc::parseit
