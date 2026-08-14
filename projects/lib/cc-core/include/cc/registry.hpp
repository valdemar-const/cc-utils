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
// `pipeline_dir` (possibly empty) lets path-bearing editors resolve relative
// entries against the .pipeline file's directory before validating.
// Returning an error surfaces the validator's message next to the control.
using value_parse_fn = std::function<auto(std::string_view text, std::string_view pipeline_dir)->std::expected<any_value, std::string>>;

// Descriptor of a pin value type (a "connection type" of the DSL).
// Registered by PIPELINE plugins at load time — pure vocabulary: the name,
// the short pin annotation and a description. No editor concerns live here:
// inline editors are a HOST/workbench-layer extension registered separately
// via type_registry::register_inline_editor(), keyed by the type name. This
// keeps "which types exist" (pipeline side) decoupled from "how to edit
// them" (editor side) — every pin of a given type behaves identically on
// every node.
struct value_type_desc
{
    std::string_view name;       // canonical PascalCase name, globally unique
    std::string_view short_name; // compact pin annotation ("str")
    std::string_view description;
};

// Type registry: maps pin-type names to runtime descriptors. Populated at
// plugin load time as each plugin registers the value types it introduces.
// The host queries it to render the canvas (names, colours, pin annotations)
// and — through the host-registered inline editors — to parse inline-edited
// pin values.
class CC_CORE_API type_registry
{
  public:

    virtual ~type_registry();

    // Register a concrete value type T under its canonical name.
    // Idempotent: re-registering the same (name, T) pair with an identical
    // descriptor is a no-op; a mismatched name↔T or conflicting short name
    // is a hard error (returns false → plugin misbehaviour, surfaced by the
    // loader).
    template<typename T>
        requires std::is_copy_constructible_v<std::remove_cvref_t<T>>
    auto
    register_value_type(value_type_desc d) -> bool
    {
        return register_value_type_impl(std::move(d), descriptor_of<T>);
    }

    // Register an inline editor for every pin of the named connection type,
    // regardless of which node owns the pin. Host/workbench-layer extension
    // point (pipeline plugins do not call this): the editor pack decides how
    // values of each type are typed in by hand. First registration wins;
    // re-registering the same (name, control) pair is a no-op, a conflicting
    // one fails. May be called before or after the type itself is
    // registered (lookup resolves through the canonical name).
    virtual auto register_inline_editor(std::string_view type_name, property_kind control, value_parse_fn parse) -> bool = 0;

    // Register a NAMED value editor ("open with editor…") for every pin of
    // the named connection type. Same host-layer extension point as inline
    // editors, but an open catalog: a type may offer several editors, and
    // the workbench lists them when the user opens a pin's value in an
    // Editor tab. The registry stores only the type name → editor ids
    // mapping; the editor widgets themselves are workbench-side. Deduped,
    // order-preserving, empty names ignored.
    virtual auto register_value_editor(std::string_view type_name, std::string_view editor_name) -> void = 0;

    // Lookup by descriptor → canonical name, or empty string_view if unknown.
    virtual auto name_of(type_descriptor_t d) const -> std::string_view = 0;

    // Lookup by descriptor → short pin annotation, or empty if unknown / none.
    virtual auto short_name_of(type_descriptor_t d) const -> std::string_view = 0;

    // Lookup by name → descriptor, or default-constructed descriptor if unknown.
    virtual auto descriptor_of_name(std::string_view name) const -> type_descriptor_t = 0;

    // Connect-check: may a value of type `out` flow into an input expecting `in`?
    // True for exact match, true if `in` is the wildcard Any, false otherwise.
    virtual auto is_connectable(type_descriptor_t out, type_descriptor_t in) const -> bool = 0;

    // Inline editor kind for values of this type, or nullopt if the type is
    // not inline-editable (no editor registered / unknown type).
    virtual auto inline_editor_of(type_descriptor_t d) const -> std::optional<property_kind> = 0;

    // Parse inline-edited text into a typed value. Fails if the type has no
    // inline editor, the validator rejects the text, or the type is unknown.
    virtual auto parse_value(type_descriptor_t d, std::string_view text, std::string_view pipeline_dir) const
            -> std::expected<any_value, std::string> = 0;

    // Editor ids offered for values of this type, in registration order;
    // empty for unknown or editor-less types. The returned views stay valid
    // as long as the registry lives.
    virtual auto value_editors_of(type_descriptor_t d) const -> std::vector<std::string_view> = 0;

  protected:

    virtual auto register_value_type_impl(value_type_desc d, type_descriptor_t t) -> bool = 0;
};

} // namespace cc
