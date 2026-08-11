#pragma once

#include "cc-core_export.hpp"
#include "cc/any_value.hpp"

#include <string_view>
#include <type_traits>

namespace cc {

// Type registry: maps human-readable pin-type names ("text", "ast.tl", "ir",
// "bytes", "any") to runtime descriptors. Populated at plugin load time as
// each plugin registers the value types it introduces. The host queries it
// to render the canvas (names, colours per type) and to validate wires.
//
// For MVP: minimal interface covering registration + lookup + the one
// wildcard rule ("any" accepts everything, used by the View node). Subtype
// hierarchy + registered coercions land later.
class CC_CORE_API type_registry {
 public:
  virtual ~type_registry();

  // Register a concrete value type T under a stable name.
  // Idempotent: re-registering the same pair is a no-op; mismatched name↔T
  // is a hard error (returns false → plugin load fails).
  template <typename T>
  requires std::is_copy_constructible_v<std::remove_cvref_t<T>>
  auto register_value_type(std::string_view name) -> bool {
    return register_value_type_impl(name, descriptor_of<T>);
  }

  // Lookup by descriptor → registered name, or empty string_view if unknown.
  virtual auto name_of(type_descriptor_t d) const -> std::string_view = 0;

  // Lookup by name → descriptor, or descriptor_of<void> if unknown.
  virtual auto descriptor_of_name(std::string_view name) const -> type_descriptor_t = 0;

  // Connect-check: may a value of type `out` flow into an input expecting `in`?
  // True for exact match, true if `in` is the wildcard "any", false otherwise.
  // (Subtype hierarchy + coercions extend this later.)
  virtual auto is_connectable(type_descriptor_t out, type_descriptor_t in) const -> bool = 0;

 protected:
  virtual auto register_value_type_impl(std::string_view name, type_descriptor_t d) -> bool = 0;
};

}  // namespace cc
