#pragma once

#include "cc-core_export.hpp"
#include "cc/any_value.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace cc {

// Opaque draw context handed to a view renderer. For MVP it is an empty
// anchor — the workbench fills it in (ImGui draw list, viewport rect, etc.)
// as the UI takes shape. Defined as an abstract base so ABI stays stable.
class CC_CORE_API view_context {
 public:
  virtual ~view_context();
};

// Renders one value into a view panel. Plugins that introduce a pin type
// typically also register an IViewRenderer for it. A renderer may also be
// registered for a type it does not own (e.g. a hex viewer for foreign bytes).
//
// Implementations resolve the concrete type via aa::any_cast<T> inside.
class CC_CORE_API view_renderer {
 public:
  virtual ~view_renderer();

  // Which type this renderer handles. Used by the provider for dispatch.
  virtual auto type_name() const -> std::string_view = 0;

  // Render `value` into `ctx`. Called by the host's View panel.
  virtual auto render(const any_value& value, view_context& ctx) -> void = 0;
};

// Resolves a renderer for a value's runtime type. The host calls
// `get_for_type(value.type_descriptor())->render(value, ctx)` — exactly the
// dispatch shape from the design notes.
//
// Lookup order: exact-type match → (future: subtype) → fallback default
// renderer that just prints the type name + raw bytes count.
class CC_CORE_API view_renderer_provider {
 public:
  virtual ~view_renderer_provider();

  virtual auto get_for_type(type_descriptor_t type) const -> view_renderer* = 0;

  // Enumerate every registered renderer (for diagnostics / settings UI).
  virtual auto all() const -> std::span<view_renderer* const> = 0;

  virtual auto register_renderer(std::unique_ptr<view_renderer> r) -> void = 0;
};

}  // namespace cc
