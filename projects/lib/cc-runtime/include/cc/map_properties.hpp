#pragma once

// Concrete node_properties implementation: free-form string-keyed map.
//
// Plugins that need richer property storage (typed schemas, validation, ...)
// subclass node_properties directly; this default is enough for most nodes
// and is what node_factory::create() typically wires up.

#include "cc-runtime_export.hpp"
#include "cc/node.hpp"

#include <string>
#include <unordered_map>

namespace cc::runtime {

class CC_RUNTIME_API map_properties final : public node_properties {
 public:
  auto get(std::string_view key) const -> std::string_view override;
  auto set(std::string_view key, std::string_view value) -> void override;

  // Direct map access (for serialisation, etc.).
  auto storage() const -> const std::unordered_map<std::string, std::string>& { return storage_; }
  auto storage() -> std::unordered_map<std::string, std::string>& { return storage_; }

 private:
  std::unordered_map<std::string, std::string> storage_;
};

}  // namespace cc::runtime
