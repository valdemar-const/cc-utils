#pragma once

#include "cc-astq_export.hpp"

#include <string_view>

namespace cc::astq {

[[nodiscard]] CC_ASTQ_API std::string_view version() noexcept;

}  // namespace cc::astq
