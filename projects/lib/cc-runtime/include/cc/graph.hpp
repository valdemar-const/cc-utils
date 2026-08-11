#pragma once

#include "cc-runtime_export.hpp"
#include "cc/node.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc::runtime
{

// One wire between an output slot and an input slot. Slot ids are local to
// the node that owns them.
struct CC_RUNTIME_API edge
{
    std::string src_node;
    std::string src_slot;
    std::string dst_node;
    std::string dst_slot;
};

// Mutable in-memory node graph. Owns all node instances.
// For MVP: vectors + linear lookup by instance_id. No serialisation yet.
class CC_RUNTIME_API graph
{
  public:

    graph();
    ~graph();

    graph(const graph &)            = delete;
    graph &operator=(const graph &) = delete;

    // Take ownership of a node. The node's instance_id() must be unique in the
    // graph (caller's responsibility — node_factory generates fresh ids).
    void add_node(std::unique_ptr<node> n);

    // Connect (src_node.src_slot) -> (dst_node.dst_slot). Does not validate
    // types or existence of nodes/slots at this layer; the host checks types
    // before adding edges.
    void add_edge(edge e);

    // Remove all edges touching a node (helper for node deletion).
    void remove_edges_of(std::string_view instance_id);

    // Remove a node by instance_id. Returns true if removed.
    // Caller should remove_edges_of() first to keep the graph consistent.
    auto remove_node(std::string_view instance_id) -> bool;

    // Remove the edge matching (src_node, src_slot, dst_node, dst_slot).
    auto remove_edge(std::string_view src_node, std::string_view src_slot, std::string_view dst_node, std::string_view dst_slot) -> bool;

    // Look up by instance_id. Returns nullptr if not found.
    auto find_node(std::string_view instance_id) const -> node *;
    auto find_node(std::string_view instance_id) -> node *;

    // Find the upstream source feeding (dst_node, dst_slot). Returns
    // {src_node, src_slot} or nullopt if nothing is connected.
    auto find_source(std::string_view dst_node, std::string_view dst_slot) const
            -> std::optional<std::pair<std::string, std::string>>;

    auto
    nodes() const -> std::span<const std::unique_ptr<node>>
    {
        return nodes_;
    }

    auto
    edges() const -> std::span<const edge>
    {
        return edges_;
    }

  private:

    std::vector<std::unique_ptr<node>> nodes_;
    std::vector<edge>                  edges_;
};

} // namespace cc::runtime
