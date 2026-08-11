#pragma once

#include "cc-core_export.hpp"
#include "cc/registry.hpp"
#include "cc/view.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace cc {

class node_factory;  // see cc/node_factory.hpp

// Host-side collection point that plugins populate at load time. Aggregates:
//  - the type registry (pin types)
//  - the node factory catalogue (node types)
//  - the view renderer provider (how to draw each value type)
//
// Abstract: concrete implementation lives in cc-runtime; the abstract surface
// here is what plugins program against. The host (workbench) also uses this
// interface to enumerate registered node types for the canvas context menu
// and to look up view renderers for the View panel.
class CC_CORE_API host_registry {
 public:
  virtual ~host_registry();

  // ---- type registry ----
  virtual auto types() -> type_registry& = 0;
  virtual auto types() const -> const type_registry& = 0;

  // ---- node factories ----
  virtual auto register_node_factory(std::unique_ptr<node_factory> factory) -> void = 0;
  virtual auto find_node_factory(std::string_view type_id) const -> node_factory* = 0;

  // All registered factories, in registration order. Used by the host to
  // populate the canvas context menu.
  virtual auto node_factories() const -> std::span<node_factory* const> = 0;

  // ---- view renderers ----
  virtual auto renderers() -> view_renderer_provider& = 0;

  // ---- plugin provider bookkeeping -------------------------------------
  // Used by the plugin_loader around cc_plugin_register(): set the name of the
  // plugin currently being registered so that register_node_factory() can
  // attribute the factory to it. Calling push_provider() / pop_provider() in
  // matched pairs, the innermost name wins. Public so that host-side code
  // (loader) can drive attribution; plugins themselves never call these.
  //
  // get_provider_of() looks up which plugin contributed a given node type.
  // Returns an empty string_view for node types registered outside any
  // provider scope (e.g. host-side built-ins) or for unknown type ids.
  //
  // loaded_plugins() returns the names of all plugins for which
  // push_provider()/pop_provider() have ever been entered, in first-load
  // order. Used by the workbench to validate <requires> in a .pipeline file.
  virtual auto push_provider(std::string_view name)  -> void = 0;
  virtual auto pop_provider()                        -> void = 0;
  virtual auto provider_of(std::string_view type_id) const -> std::string_view = 0;
  virtual auto loaded_plugins() const -> std::span<const std::string> = 0;
};

}  // namespace cc
