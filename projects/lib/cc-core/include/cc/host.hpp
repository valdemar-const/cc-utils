#pragma once

#include "cc-core_export.hpp"
#include "cc/domain.hpp"
#include "cc/registry.hpp"
#include "cc/view.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cc
{

class node_factory; // see cc/node_factory.hpp

// Host-side collection point that plugins populate at load time. Aggregates:
//  - the type registry (pin types)
//  - the vocabulary domain registry (see cc/domain.hpp)
//  - the node factory catalogue (node types)
//  - the view renderer provider (how to draw each value type)
//
// Abstract: concrete implementation lives in cc-runtime; the abstract surface
// here is what plugins program against. The host (workbench) also uses this
// interface to enumerate registered node types for the canvas context menu
// and to look up view renderers for the View panel.
class CC_CORE_API host_registry
{
  public:

    virtual ~host_registry();

    // ---- type registry ----
    virtual auto types() -> type_registry &             = 0;
    virtual auto types() const -> const type_registry & = 0;

    // ---- vocabulary domains -----------------------------------------------
    // Open-world registry of domain descriptors. register_domain() merges by
    // id: dependencies union, first non-empty display metadata wins — so any
    // plugin may seed or extend any domain regardless of load order.
    //
    // push_domain()/pop_domain() set the domain any types registered in
    // between are attributed to (mirrors push_provider/pop_provider). Used by
    // domain-provider plugins around their register_value_type() calls;
    // provided_types is informational (New Pipeline dialog).
    virtual auto register_domain(domain_desc d) -> void                                                    = 0;
    virtual auto find_domain(std::string_view id) const -> const domain_desc *                             = 0;
    virtual auto domains() const -> std::span<const domain_desc>                                           = 0;
    virtual auto domain_closure(std::span<const std::string_view> roots) const -> std::vector<std::string> = 0;
    virtual auto push_domain(std::string_view id) -> void                                                  = 0;
    virtual auto pop_domain() -> void                                                                      = 0;

    // ---- node factories ----
    virtual auto register_node_factory(std::unique_ptr<node_factory> factory) -> void = 0;
    virtual auto find_node_factory(std::string_view type_id) const -> node_factory *  = 0;

    // All registered factories, in registration order. Used by the host to
    // populate the canvas context menu.
    virtual auto node_factories() const -> std::span<node_factory * const> = 0;

    // ---- view renderers ----
    virtual auto renderers() -> view_renderer_provider & = 0;

    // ---- plugin provider bookkeeping -------------------------------------
    // Used by the plugin_loader around cc_plugin_register(): set the name of the
    // plugin currently being registered so that register_node_factory() can
    // attribute the factory to it. Calling push_provider() / pop_provider() in
    // matched pairs, the innermost name wins. Public so that host-side code
    // (loader) can drive attribution; plugins themselves never call these.
    //
    // provider_of() looks up which plugin contributed a given node type.
    // Returns an empty string_view for node types registered outside any
    // provider scope (e.g. host-side built-ins) or for unknown type ids.
    //
    // loaded_plugins() returns the names of all plugins for which
    // push_provider()/pop_provider() have ever been entered, in first-load
    // order. Used by the workbench to validate <requires> in a .pipeline file.
    virtual auto push_provider(std::string_view name) -> void                    = 0;
    virtual auto pop_provider() -> void                                          = 0;
    virtual auto provider_of(std::string_view type_id) const -> std::string_view = 0;
    virtual auto loaded_plugins() const -> std::span<const std::string>          = 0;
};

} // namespace cc
