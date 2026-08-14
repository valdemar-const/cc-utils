#pragma once

// .pipeline file format (XML) — save/load a runtime graph to disk together
// with the canvas layout (node positions), the vocabulary domain contract,
// and a <requires> section listing every plugin whose node types appear in
// the graph.
//
// Domain contract (v2): a pipeline is created inside one root vocabulary
// domain and may import additional domains (add-only). The set is immutable
// for the file's lifetime — the loader restores it verbatim and never
// mutates it. A v1 file (no domain attribute) loads in legacy mode: every
// factory is visible and a warning asks the user to re-save.
//
// Inline pin values are stored per node in a <values> section (raw texts
// keyed by input slot id), separate from <properties> — different namespaces,
// different consumers (runner vs node logic).
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

namespace cc::workbench
{

// On-disk position in canvas-local pixels. Matches imgui-node-editor's
// editor-space coordinates (the same space SetNodePosition / GetNodePosition
// operate in).
struct pos
{
    float x = 0.0f;
    float y = 0.0f;
};

// Format version. Bumped whenever the schema changes in a backward-
// incompatible way. The version is written to the <pipeline version="N">
// root attribute and checked on load.
//
//   v1: no domain attribute (legacy — loads with all factories visible)
//   v2: domain="..." + <imports> + per-node <values>
inline constexpr int k_pipeline_format_version = 2;

// The file's vocabulary-domain contract. `root` is the domain the pipeline
// was created in (immutable); `imports` are extra domains added after
// creation (add-only). Both are written to / restored from disk verbatim.
struct pipeline_domains
{
    std::string              root;
    std::vector<std::string> imports;
};

// Soft warnings produced while loading — the graph is still usable, but the
// user should be told something is off (e.g. a required plugin is missing and
// nodes of that plugin were skipped).
struct load_warnings
{
    std::vector<std::string> missing_plugins;    // declared in <requires>, not loaded
    std::vector<std::string> unknown_node_types; // no factory for this type_id
    std::vector<std::string> skipped_edges;      // reference an unknown instance_id
    std::vector<std::string> missing_domains;    // imported domains not registered by any loaded plugin
};

// Result of a successful load: the domain contract to restore in the host,
// per-instance positions, legacy flag (v1 file) + warnings.
struct load_result
{
    pipeline_domains                     domains;
    bool                                 legacy = false;
    std::unordered_map<std::string, pos> positions;
    load_warnings                        warnings;
};

// Write the graph + positions + domain contract to `path`. Collects the
// unique set of plugin providers for the <requires> section by asking the
// host which plugin registered each node type.
//
// Property and inline-value texts are written verbatim — no path
// relativisation, no canonicalisation. Round-trip is identity-preserving; a
// relative path the user typed stays relative on disk and after load.
// Resolution of relative paths against the pipeline's directory happens at
// activate() time via activate_context, not at the storage layer.
//
// Returns an error string on I/O / serialisation failure; success is void.
auto save_pipeline(const host_registry &host, const runtime::graph &g, const std::unordered_map<std::string, pos> &positions, const pipeline_domains &domains, const std::string &path) -> std::expected<void, std::string>;

// Read a pipeline XML file into `g` (clears it first) and returns the domain
// contract, positions to restore + any soft warnings. Hard failures (file
// missing, malformed XML, bad version, unavailable root domain) return an
// error string and leave `g` cleared.
//
// v1 files load in legacy mode: empty domain contract, legacy=true, and a
// warning prompting a re-save. Version >2 is refused.
//
// Nodes whose type_id has no factory in `host` are skipped; edges referencing
// skipped nodes are also skipped. Imported domains that no loaded plugin
// registers produce missing_domains warnings (the graph still loads).
auto load_pipeline(const host_registry &host, runtime::graph &g, const std::string &path) -> std::expected<load_result, std::string>;

} // namespace cc::workbench
