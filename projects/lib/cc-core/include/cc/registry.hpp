#pragma once

#include "cc-core_export.hpp"
#include "cc/any_value.hpp"
#include "cc/property_kind.hpp"

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace cc
{

// Parses the text of an inline-edited pin value into a typed any_value.
// Returning an error surfaces the validator's message next to the control.
// The callable is registered by the plugin that owns the type and must remain
// valid for the host's lifetime (plugins are loaded once, never unloaded).
using value_parse_fn = std::function<auto(std::string_view text)->std::expected<any_value, std::string>>;

// Descriptor of a pin value type. Registered by plugins at load time.
//
// Naming convention: `name` is the canonical PascalCase name ("String"),
// globally unique in the registry; `short_name` is the compact pin annotation
// ("str") shown next to pin labels on the canvas (`name:short`). Short names
// are a display convention — uniqueness is recommended, not enforced; a
// collision is disambiguated by the tooltip showing the full name.
struct value_type_desc
{
    std::string_view name;
    std::string_view short_name;
    std::string_view description;

    // When set, unconnected input pins of this type are editable in place in
    // the node body: the host renders the control matching the kind, stores
    // the raw text in the node's slot_values() map, and the runner parses it
    // via `parse` and injects the result as a regular input value. Opaque
    // types (File, Ast, Module) leave this unset — they only originate from
    // nodes.
    std::optional<property_kind> inline_control;
    value_parse_fn               parse; // required when inline_control is set
};

// Type registry: maps pin-type names to runtime descriptors. Populated at
// plugin load time as each plugin registers the value types it introduces.
// The host queries it to render the canvas (names, colours, pin annotations),
// to validate wires, and to parse inline-edited pin values.
class CC_CORE_API type_registry
{
  public:

    virtual ~type_registry();

    // Register a concrete value type T under its canonical name.
    // Idempotent: re-registering the same (name, T) pair with an identical
    // descriptor is a no-op; a mismatched name↔T or conflicting descriptor
    // fields is a hard error (returns false → plugin misbehaviour, surfaced
    // by the loader).
    template<typename T>
        requires std::is_copy_constructible_v<std::remove_cvref_t<T>>
    auto
    register_value_type(value_type_desc d) -> bool
    {
        if (d.inline_control.has_value() && !d.parse)
        {
            return false; // inline-editable type must provide a parser
        }
        return register_value_type_impl(std::move(d), descriptor_of<T>);
    }

    // Lookup by descriptor → canonical name, or empty string_view if unknown.
    virtual auto name_of(type_descriptor_t d) const -> std::string_view = 0;

    // Lookup by descriptor → short pin annotation, or empty if unknown / none.
    virtual auto short_name_of(type_descriptor_t d) const -> std::string_view = 0;

    // Lookup by name → descriptor, or default-constructed descriptor if unknown.
    virtual auto descriptor_of_name(std::string_view name) const -> type_descriptor_t = 0;

    // Connect-check: may a value of type `out` flow into an input expecting `in`?
    // True for exact match, true if `in` is the wildcard Any, false otherwise.
    virtual auto is_connectable(type_descriptor_t out, type_descriptor_t in) const -> bool = 0;

    // Inline editor for values of this type, or nullopt if the type is not
    // inline-editable.
    virtual auto inline_editor_of(type_descriptor_t d) const -> std::optional<property_kind> = 0;

    // Parse inline-edited text into a typed value. Fails if the type has no
    // inline editor, the validator rejects the text, or the type is unknown.
    virtual auto parse_value(type_descriptor_t d, std::string_view text) const
            -> std::expected<any_value, std::string> = 0;

  protected:

    virtual auto register_value_type_impl(value_type_desc d, type_descriptor_t t) -> bool = 0;
};

} // namespace cc
