#pragma once

#include "cc-runtime_export.hpp"
#include "cc/host.hpp"

#include <memory>

namespace cc::runtime {

// Concrete host_registry factory. Returns an empty registry ready for the
// plugin loader to populate.
[[nodiscard]] CC_RUNTIME_API auto make_host_registry() -> std::unique_ptr<host_registry>;

}  // namespace cc::runtime
