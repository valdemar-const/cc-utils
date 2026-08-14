#pragma once

#include "cc-core_export.hpp"

#include <string_view>

namespace cc
{

// Bumped on any change to the cc-core plugin contract (interfaces in
// cc/host.hpp, cc/node_factory.hpp, cc/node.hpp, cc/any_value.hpp).
// Host refuses mismatched plugins at load time with a "rebuild plugin" error.
//
// v3: new plugin model — single cc_plugin_register(host_registry&) entry
//     replaces the old per-stage factories. AnyAny-backed any_value carrier.
// v4: vocabulary domains (node_factory::domains, host_registry domain API,
//     value_type_desc with short names + inline editors) and
//     node::slot_values() for inline-edited pin values.
inline constexpr int plugin_api_version = 4;

// Static metadata returned by cc_plugin_load() for cheap scanning.
struct plugin_info
{
    int         api_version;
    const char *name; // short id, e.g. "tl", "basic", "x86_64"
    const char *kind; // informational: "frontend" | "backend" | "basic" | "io" ...
};

class host_registry; // forward — see cc/host.hpp

} // namespace cc

// Plugin entry points (extern "C", resolved by the host via dlopen/Boost.DLL).
//
// cc_plugin_load()     — cheap: returns metadata + version. Host refuses
//                        mismatched api_version without calling register.
// cc_plugin_register() — heavy: plugin populates the host_registry with its
//                        types, node factories, view renderers, etc.
//                        Called once at load time, after a successful version
//                        check via cc_plugin_load.
extern "C" cc::plugin_info cc_plugin_load();
extern "C" void            cc_plugin_register(cc::host_registry &registry);
