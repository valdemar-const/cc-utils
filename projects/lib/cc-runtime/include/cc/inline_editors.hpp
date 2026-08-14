#pragma once

// Host-side default inline-editor pack for pin values.
//
// Inline editors are a HOST/workbench-layer extension, NOT a pipeline-plugin
// concern: pipeline plugins register pure connection types
// (cc::value_type_desc = name + short annotation), and the host decides how
// values of each type are entered by hand. register_inline_editors() wires
// one editor per well-known type name, so every unconnected input pin of a
// given type — on every node, from every vendor — behaves identically.
//
// A dedicated workbench-plugin API may register further editors through the
// same type_registry::register_inline_editor() entry point later.

#include "cc-runtime_export.hpp"
#include "cc/host.hpp"

namespace cc::runtime
{

// Register editors for the well-known connection types: String, Integer,
// Double, Boolean, Path (verbatim text) and File (text resolved against
// pipeline_dir, then stat-validated into a cc::fs::file_handle). Call once
// after plugin loading; idempotent. Types that are not registered by any
// loaded plugin are skipped silently.

CC_RUNTIME_API auto register_inline_editors(host_registry &host) -> void;

// Register the default catalog of named value editors ("open with
// editor…") — the rich counterparts of the inline controls, opened by the
// workbench in dedicated Editor tabs. Same host-layer entry point as above:
// the registry only maps resource type name → editor ids; the editor
// widgets themselves live in the workbench and dispatch on the id. Ids are
// globally unique and namespaced ("editors.<family>.<flavour>"), and one
// id may serve several types. Today: "editors.text.plain" (multiline) and
// "editors.text.code" (syntax-highlighted) for String;
// "editors.text.plain" for Path (footer path properties open there too).
// Tabs share the one model — the node's slot_values()/properties() text —
// so several editors (even identical ones, or the inline pin control)
// stay in sync automatically.

CC_RUNTIME_API auto register_value_editors(host_registry &host) -> void;

} // namespace cc::runtime
