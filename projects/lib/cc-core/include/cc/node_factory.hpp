#pragma once

#include "cc-core_export.hpp"
#include "cc/node.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace cc
{

// Property kind hints for the workbench's property editor. The host picks the
// widget that matches the kind; plugins declare one per property in the
// factory's property_schema().
enum class property_kind
{
    text,      // single-line InputText
    multiline, // multi-line InputTextMultiline
    path,      // InputText + "..." Browse button (opens ImFileDialog)
    integer,   // InputInt
    boolean,   // Checkbox
};

// One property descriptor: stable key, display name, kind, default value.
// The workbench reads the schema to render property widgets generically,
// without hardcoding per node-type.
struct property_desc
{
    std::string_view key;          // "path", "name" — used in properties().get/set
    std::string_view display_name; // "File Path"   — shown in the editor
    property_kind    kind;
    std::string_view default_value;
};

// Factory for one node type. Plugins register one factory per node type they
// contribute; the host uses factories to instantiate fresh nodes when a
// pipeline is created or loaded from disk.
//
// Identity + visual metadata (type_id / display_name / category / property
// schema) is constant per factory; slots + property values live on each
// created node instance.
class CC_CORE_API node_factory
{
  public:

    virtual ~node_factory();

    // Stable unique id, e.g. "basic.text.from_file", "tl.frontend".
    virtual auto type_id() const -> std::string_view = 0;

    // Human-readable name for menus, e.g. "Source File".
    virtual auto display_name() const -> std::string_view = 0;

    // Grouping for the canvas context menu, e.g. "Basic", "TL", "Backend".
    // The host groups factories by this string when building the popup.
    virtual auto category() const -> std::string_view = 0;

    // Property schema: descriptors of properties this node type exposes. The
    // host uses this to render generic property editors in the canvas. Default
    // is empty (no editable properties).
    virtual auto
    property_schema() const -> std::span<const property_desc>
    {
        return {};
    }

    // Create a fresh node instance with default property values and a freshly
    // generated instance_id.
    virtual auto create() const -> std::unique_ptr<node> = 0;

    // Create a node instance reusing an explicit instance_id. Used when loading
    // a pipeline from disk: edges reference instance_ids, so the created nodes
    // must keep those exact ids. Implementations must not generate a new id.
    // Implementations should accept any non-empty instance_id, even if its
    // suffix doesn't follow this factory's usual scheme.
    virtual auto create_with_id(std::string_view instance_id) const
            -> std::unique_ptr<node> = 0;

  protected:

    // Helper for subclasses: stamp the schema's default property values onto a
    // freshly constructed node and return it. Both create() and create_with_id()
    // share this to keep property initialisation in one place.
    auto
    apply_defaults(std::unique_ptr<node> n) const -> std::unique_ptr<node>
    {
        for (const auto &d : property_schema())
        {
            n->properties().set(d.key, d.default_value);
        }
        return n;
    }
};

} // namespace cc
