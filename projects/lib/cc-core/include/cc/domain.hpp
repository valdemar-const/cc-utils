#pragma once

#include <string>
#include <vector>

namespace cc
{

// One vocabulary domain ("предметная область") of the visual pipeline DSL.
//
// A domain is an open registry of node types and value types — like a palette
// in a design tool. Whoever seeds it (usually the plugin that first calls
// register_domain) does NOT own it: any plugin may contribute nodes or types
// to any domain, and declare membership from its own node factories.
//
// `depends_on` lists other domains whose vocabulary is visible whenever this
// domain is visible (transitively). The dot/slash path in ids
// ("compiler/lang/tl") is a naming convention, not a hierarchy — visibility
// is defined only by these explicit edges.
//
// `provided_types` is filled automatically by the host: value types
// registered inside a push_domain()/pop_domain() scope are attributed to that
// domain. Informational — used by the New Pipeline dialog.
struct domain_desc
{
    std::string              id;           // "basic/types", "filesystem"
    std::string              display_name; // "Basic Types"
    std::string              description;
    std::vector<std::string> depends_on;
    std::vector<std::string> provided_types;
};

} // namespace cc
