#pragma once

// .pipeline file format (XML) — save/load a runtime graph to disk together
// with the canvas layout (node positions) and a <requires> section listing
// every plugin whose node types appear in the graph. The host uses the
// requires list on load to warn early when a declared plugin is not present,
// so the user sees "install cc-plugin-tl" instead of cryptic "unknown node
// type" failures mid-graph.
//
// The module is deliberately ImGui-free: positions are exposed as a small
// plain struct so the serializer can be unit-tested without dragging in the
// UI headers. The workbench converts between ImVec2 and pipeline_xml::pos at
// the call site.

#include "cc/host.hpp"
#include "cc/graph.hpp"

#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

namespace cc::workbench {

// On-disk position in canvas-local pixels. Matches imgui-node-editor's
// editor-space coordinates (the same space SetNodePosition / GetNodePosition
// operate in).
struct pos {
  float x = 0.0f;
  float y = 0.0f;
};

// Format version. Bumped whenever the schema changes in a backward-
// incompatible way. The version is written to the <pipeline version="N">
// root attribute and checked on load.
inline constexpr int k_pipeline_format_version = 1;

// Soft warnings produced while loading — the graph is still usable, but the
// user should be told something is off (e.g. a required plugin is missing and
// nodes of that plugin were skipped).
struct load_warnings {
  std::vector<std::string> missing_plugins;       // declared in <requires>, not loaded
  std::vector<std::string> unknown_node_types;    // no factory for this type_id
  std::vector<std::string> skipped_edges;         // reference an unknown instance_id
};

// Result of a successful load: per-instance positions to restore + warnings.
struct load_result {
  std::unordered_map<std::string, pos> positions;
  load_warnings warnings;
};

// Write the graph + positions to `path`. Collects the unique set of plugin
// providers for the <requires> section by asking the host which plugin
// registered each node type. Nodes whose factory is unknown (shouldn't
// normally happen — every node in `g` was created from a factory) are still
// written with type_id but contribute no <requires> entry.
//
// Returns an error string on I/O / serialisation failure; success is void.
auto save_pipeline(const host_registry& host,
                   const runtime::graph& g,
                   const std::unordered_map<std::string, pos>& positions,
                   const std::string& path) -> std::expected<void, std::string>;

// Read a pipeline XML file into `g` (clears it first) and returns the
// positions to restore + any soft warnings. Hard failures (file missing,
// malformed XML, bad version) return an error string and leave `g` cleared.
//
// Nodes whose type_id has no factory in `host` are skipped; edges referencing
// skipped nodes are also skipped. Both are reported in load_warnings so the
// caller can surface them in a modal.
auto load_pipeline(const host_registry& host,
                   runtime::graph& g,
                   const std::string& path) -> std::expected<load_result, std::string>;

}  // namespace cc::workbench
