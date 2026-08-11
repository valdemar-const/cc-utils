#include "pipeline_xml.hpp"

#include "cc/node.hpp"
#include "cc/node_factory.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace cc::workbench
{

namespace
{

    // --------------------------------------------------------------------------
    // XML element construction helpers.
    // --------------------------------------------------------------------------

    auto
    append_attribute(pugi::xml_node node, const char *name, std::string_view value) -> void
    {
        // pugixml wants a null-terminated string; build one explicitly.
        node.append_attribute(name).set_value(std::string {value}.c_str());
    }

    auto
    append_float_attribute(pugi::xml_node node, const char *name, float v) -> void
    {
        // %g is compact and round-trips for our coordinate range.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        node.append_attribute(name).set_value(buf);
    }

    auto
    parse_float_attr(const pugi::xml_attribute &a, float &out) -> bool
    {
        if (a.empty())
        {
            return false;
        }
        try
        {
            out = std::stof(a.value());
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // --------------------------------------------------------------------------
    // <requires> — unique set of plugin providers for every node type in the graph.
    // --------------------------------------------------------------------------
    auto
    collect_required_plugins(const host_registry &host, const runtime::graph &g)
            -> std::vector<std::string>
    {
        std::set<std::string> uniq;
        for (const auto &n : g.nodes())
        {
            auto provider = host.provider_of(n->type_id());
            if (!provider.empty())
            {
                uniq.emplace(provider);
            }
        }
        return {uniq.begin(), uniq.end()};
    }

    auto
    write_requires(pugi::xml_node root, const std::vector<std::string> &plugins) -> void
    {
        auto req = root.append_child("requires");
        for (const auto &name : plugins)
        {
            auto p = req.append_child("plugin");
            append_attribute(p, "name", name);
        }
    }

    // --------------------------------------------------------------------------
    // <nodes> — every node with type, instance id, optional <pos>, <properties>.
    // --------------------------------------------------------------------------
    auto
    write_nodes(pugi::xml_node root, const host_registry &host, const runtime::graph &g, const std::unordered_map<std::string, pos> &positions) -> void
    {
        auto nodes = root.append_child("nodes");
        for (const auto &n : g.nodes())
        {
            auto node_el = nodes.append_child("node");
            append_attribute(node_el, "type", n->type_id());
            append_attribute(node_el, "id", n->instance_id());

            if (auto it = positions.find(std::string {n->instance_id()}); it != positions.end())
            {
                auto pos_el = node_el.append_child("pos");
                append_float_attribute(pos_el, "x", it->second.x);
                append_float_attribute(pos_el, "y", it->second.y);
            }

            auto props_el = node_el.append_child("properties");
            // Iterate the factory's property schema — it is the authoritative list of
            // keys this node type exposes, so we round-trip exactly the editable set
            // the UI shows (no internal fields leaking, no missing fields). Values
            // are written verbatim: no path relativisation, no canonicalisation —
            // see save_pipeline's contract comment for the rationale.
            if (auto *f = host.find_node_factory(n->type_id()))
            {
                for (const auto &desc : f->property_schema())
                {
                    std::string value {n->properties().get(desc.key)};
                    auto        prop_el = props_el.append_child("property");
                    append_attribute(prop_el, "key", desc.key);
                    prop_el.append_child(pugi::node_pcdata).set_value(value.c_str());
                }
            }
        }
    }

    auto
    write_edges(pugi::xml_node root, const runtime::graph &g) -> void
    {
        auto edges = root.append_child("edges");
        for (const auto &e : g.edges())
        {
            auto edge_el = edges.append_child("edge");
            append_attribute(edge_el, "src_node", e.src_node);
            append_attribute(edge_el, "src_slot", e.src_slot);
            append_attribute(edge_el, "dst_node", e.dst_node);
            append_attribute(edge_el, "dst_slot", e.dst_slot);
        }
    }

    // --------------------------------------------------------------------------
    // Load-side parsing helpers.
    // --------------------------------------------------------------------------

    auto
    read_version(const pugi::xml_document &doc) -> std::expected<int, std::string>
    {
        auto root = doc.child("pipeline");
        if (!root)
        {
            return std::unexpected<std::string>("not a .pipeline file: missing <pipeline> root");
        }
        auto v = root.attribute("version");
        if (v.empty())
        {
            return std::unexpected<std::string>("missing <pipeline version=\"...\"> attribute");
        }
        try
        {
            return std::stoi(v.value());
        }
        catch (...)
        {
            return std::unexpected<std::string>("invalid version attribute: '" + std::string {v.value()} + "'");
        }
    }

    auto
    check_requires(const host_registry &host, const pugi::xml_node &root, load_warnings &w)
            -> void
    {
        auto req = root.child("requires");
        if (!req)
        {
            return;
        }
        // Build a lookup set of currently-loaded plugin names for O(1) test.
        std::set<std::string> loaded;
        for (const auto &name : host.loaded_plugins())
        {
            loaded.emplace(name);
        }
        for (auto p = req.child("plugin"); p; p = p.next_sibling("plugin"))
        {
            auto name = p.attribute("name").value();
            if (!name || !*name)
            {
                continue;
            }
            if (loaded.find(name) == loaded.end())
            {
                w.missing_plugins.emplace_back(name);
            }
        }
    }

    auto
    read_positions(const pugi::xml_node &node_el, const std::string &instance_id, std::unordered_map<std::string, pos> &out) -> void
    {
        auto pos_el = node_el.child("pos");
        if (!pos_el)
        {
            return;
        }
        pos  p;
        bool ok_x = parse_float_attr(pos_el.attribute("x"), p.x);
        bool ok_y = parse_float_attr(pos_el.attribute("y"), p.y);
        if (ok_x && ok_y)
        {
            out[instance_id] = p;
        }
    }

    auto
    read_properties(const pugi::xml_node &node_el, cc::node &n) -> void
    {
        auto props_el = node_el.child("properties");
        if (!props_el)
        {
            return;
        }
        // Values are stored verbatim — no path resolution at load time. The runner
        // resolves relative paths against the pipeline's directory via
        // activate_context::pipeline_dir during activate(); see save_pipeline.
        for (auto prop = props_el.child("property"); prop; prop = prop.next_sibling("property"))
        {
            auto key = prop.attribute("key").value();
            if (!key || !*key)
            {
                continue;
            }
            // Use the PCDATA child as the value; falls back to the (empty) attribute
            // value if the writer ever used attribute form. Whitespace is preserved.
            std::string_view value;
            if (auto pcdata = prop.first_child(); pcdata && pcdata.type() == pugi::node_pcdata)
            {
                value = pcdata.value();
            }
            n.properties().set(key, value);
        }
    }

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

auto
save_pipeline(const host_registry &host, const runtime::graph &g, const std::unordered_map<std::string, pos> &positions, const std::string &path) -> std::expected<void, std::string>
{
    pugi::xml_document doc;
    auto               decl           = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version")  = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto root = doc.append_child("pipeline");
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", k_pipeline_format_version);
        root.append_attribute("version").set_value(buf);
    }

    write_requires(root, collect_required_plugins(host, g));
    write_nodes(root, host, g, positions);
    write_edges(root, g);

    // pretty_print writes to a std::ofstream-compatible stream; raw save_file
    // would also work but going through a stream lets us surface errno.
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os)
    {
        return std::unexpected<std::string>("cannot open '" + path + "' for writing");
    }
    unsigned flags = pugi::format_indent_attributes | pugi::format_default;
    doc.save(os, "  ", flags, pugi::encoding_utf8);
    if (!os)
    {
        // stream bad — usually means disk full or path revoked mid-write.
        return std::unexpected<std::string>("error writing '" + path + "'");
    }
    return {};
}

auto
load_pipeline(const host_registry &host, runtime::graph &g, const std::string &path) -> std::expected<load_result, std::string>
{
    if (!std::filesystem::exists(path))
    {
        return std::unexpected<std::string>("file not found: " + path);
    }

    pugi::xml_document     doc;
    pugi::xml_parse_result parse_res = doc.load_file(path.c_str(), pugi::parse_default, pugi::encoding_utf8);
    if (!parse_res)
    {
        return std::unexpected<std::string>(std::string {"XML parse error: "} + parse_res.description() + " (offset " + std::to_string(parse_res.offset) + ")");
    }

    auto version_res = read_version(doc);
    if (!version_res)
    {
        return std::unexpected(version_res.error());
    }
    if (*version_res != k_pipeline_format_version)
    {
        // Forward-compatible design: future loaders can migrate older files, but
        // currently there is only one version. Refuse loudly so the user knows.
        return std::unexpected<std::string>("unsupported pipeline version " + std::to_string(*version_res) + " (expected " + std::to_string(k_pipeline_format_version) + ")");
    }

    auto root = doc.child("pipeline");

    load_result lr;
    check_requires(host, root, lr.warnings);

    // Clear the graph up-front. On the happy path the caller will see the new
    // nodes populated below; on early-return error paths the graph stays empty.
    {
        std::vector<std::string> ids;
        ids.reserve(g.nodes().size());
        for (const auto &n : g.nodes())
        {
            ids.emplace_back(n->instance_id());
        }
        for (const auto &id : ids)
        {
            g.remove_edges_of(id);
            g.remove_node(id);
        }
    }

    // Track which instance_ids we actually created so we can skip dangling
    // edges after the node pass.
    std::set<std::string> created_ids;

    // ---- Nodes ----
    if (auto nodes = root.child("nodes"))
    {
        for (auto node_el = nodes.child("node"); node_el; node_el = node_el.next_sibling("node"))
        {
            auto type_id = node_el.attribute("type").value();
            auto inst_id = node_el.attribute("id").value();
            if (!type_id || !*type_id || !inst_id || !*inst_id)
            {
                continue;
            }

            auto *f = host.find_node_factory(type_id);
            if (!f)
            {
                lr.warnings.unknown_node_types.emplace_back(
                        std::string {type_id} + " (instance " + inst_id + ")"
                );
                continue;
            }
            auto n = f->create_with_id(inst_id);
            if (!n)
            {
                lr.warnings.unknown_node_types.emplace_back(
                        std::string {type_id} + " (instance " + inst_id + "): create_with_id returned null"
                );
                continue;
            }
            read_properties(node_el, *n);
            read_positions(node_el, inst_id, lr.positions);
            created_ids.emplace(inst_id);
            g.add_node(std::move(n));
        }
    }

    // ---- Edges ----
    if (auto edges = root.child("edges"))
    {
        for (auto edge_el = edges.child("edge"); edge_el; edge_el = edge_el.next_sibling("edge"))
        {
            auto src_node = edge_el.attribute("src_node").value();
            auto src_slot = edge_el.attribute("src_slot").value();
            auto dst_node = edge_el.attribute("dst_node").value();
            auto dst_slot = edge_el.attribute("dst_slot").value();
            if (!src_node || !dst_node)
            {
                continue;
            }
            // Skip edges that reference nodes we couldn't create (e.g. because the
            // plugin was missing). They'd dangle and break the runner.
            if (created_ids.find(src_node) == created_ids.end() || created_ids.find(dst_node) == created_ids.end())
            {
                lr.warnings.skipped_edges.emplace_back(
                        std::string {src_node} + "." + src_slot + " -> "
                        + dst_node + "." + dst_slot
                );
                continue;
            }
            g.add_edge({src_node, src_slot, dst_node, dst_slot});
        }
    }

    return lr;
}

} // namespace cc::workbench
