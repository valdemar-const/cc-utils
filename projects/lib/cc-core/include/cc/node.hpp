#pragma once

#include "cc-core_export.hpp"
#include "cc/any_value.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace cc {

enum class slot_dir  { in, out };
enum class slot_card { single, multi };

// One typed input/output slot of a node. Stable for the node instance lifetime.
// Concrete subclasses live in plugins; the abstract base anchors typeinfo here
// so the host can manipulate slot metadata without RTTI of the concrete impl.
class CC_CORE_API slot {
 public:
  virtual ~slot();
  virtual auto id()   const -> std::string_view   = 0;  // "source", "out", ...
  virtual auto type() const -> type_descriptor_t  = 0;
  virtual auto dir()  const -> slot_dir           = 0;
  virtual auto card() const -> slot_card          = 0;
};

// Diagnostic returned on activation failure. Carries a human-readable message
// (surface to logger/output tab). Will grow structured fields as needed.
struct CC_CORE_API failure {
  std::string what;
};

// Per-instance editable properties of a node (View name, file path, ...).
// Free-form string map for MVP; gets a typed schema once the .pipeline format
// stabilises. Concrete subclasses live in plugins; base anchors typeinfo.
class CC_CORE_API node_properties {
 public:
  virtual ~node_properties();
  virtual auto get(std::string_view key) const -> std::string_view = 0;
  virtual auto set(std::string_view key, std::string_view value) -> void = 0;
};

// Activation result. Either all output slots populated, or a failure carrying
// a diagnostic. The runner fills `inputs`, the node fills `outputs`.
// (Final shape of inputs/outputs is provisional; using simple spans for MVP.)
using input_pair  = std::pair<std::string_view, const any_value*>;
using output_pair = std::pair<std::string_view, any_value*>;
using activate_result = std::expected<void, failure>;

// One node in the graph. Subclassed by plugins.
//
// The host/runner owns activation timing; a node only declares its slots,
// exposes editable properties, and provides activate() which the runner calls
// with resolved inputs to produce outputs.
class CC_CORE_API node {
 public:
  virtual ~node();

  // Identity
  virtual auto type_id()     const -> std::string_view = 0;  // "tl.frontend", "view", ...
  virtual auto instance_id() const -> std::string_view = 0;  // UUID, immutable per instance

  // Topology + per-instance configuration
  virtual auto slots()      const -> std::span<slot const* const> = 0;
  virtual auto properties()       -> node_properties&            = 0;

  // Computation. Runner resolves each declared input slot id to its current
  // value (pull-based), calls activate; the node populates each declared output
  // slot's any_value* in place. Failure → runner surfaces the diagnostic and
  // stops downstream evaluation.
  virtual auto activate(std::span<const input_pair>  inputs,
                        std::span<output_pair>       outputs) -> activate_result = 0;
};

}  // namespace cc
