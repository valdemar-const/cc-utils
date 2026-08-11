#include "cc/node.hpp"
#include "cc/registry.hpp"
#include "cc/view.hpp"
#include "cc/host.hpp"
#include "cc/node_factory.hpp"

namespace cc {

// Out-of-line key functions: one shared vtable per class, owned by libcc-core.
// With CC_CORE_API visibility this guarantees typeid()/dynamic_cast on these
// abstract bases is stable across DSOs — though typical use goes through
// cc::any_value and aa::any_cast, not dynamic_cast on the INode hierarchy.

slot::~slot() = default;
node_properties::~node_properties() = default;
node::~node() = default;

type_registry::~type_registry() = default;
view_context::~view_context() = default;
view_renderer::~view_renderer() = default;
view_renderer_provider::~view_renderer_provider() = default;

host_registry::~host_registry() = default;
node_factory::~node_factory() = default;

}  // namespace cc
