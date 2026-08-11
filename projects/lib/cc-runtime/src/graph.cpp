#include "cc/graph.hpp"

#include <algorithm>  // std::remove_if
#include <utility>

namespace cc::runtime {

graph::graph()  = default;
graph::~graph() = default;

void graph::add_node(std::unique_ptr<node> n) {
  nodes_.push_back(std::move(n));
}

void graph::add_edge(edge e) {
  edges_.push_back(std::move(e));
}

void graph::remove_edges_of(std::string_view instance_id) {
  edges_.erase(
      std::remove_if(edges_.begin(), edges_.end(),
                     [&](const edge& e) {
                       return e.src_node == instance_id || e.dst_node == instance_id;
                     }),
      edges_.end());
}

auto graph::remove_node(std::string_view instance_id) -> bool {
  auto it = std::find_if(nodes_.begin(), nodes_.end(),
                         [&](const std::unique_ptr<node>& n) {
                           return n->instance_id() == instance_id;
                         });
  if (it == nodes_.end()) return false;
  nodes_.erase(it);
  return true;
}

auto graph::remove_edge(std::string_view src_node, std::string_view src_slot,
                        std::string_view dst_node, std::string_view dst_slot) -> bool {
  auto it = std::remove_if(edges_.begin(), edges_.end(),
                           [&](const edge& e) {
                             return e.src_node == src_node && e.src_slot == src_slot &&
                                    e.dst_node == dst_node && e.dst_slot == dst_slot;
                           });
  if (it == edges_.end()) return false;
  edges_.erase(it, edges_.end());
  return true;
}

auto graph::find_node(std::string_view instance_id) const -> node* {
  for (auto const& n : nodes_) {
    if (n->instance_id() == instance_id) return n.get();
  }
  return nullptr;
}

auto graph::find_node(std::string_view instance_id) -> node* {
  for (auto const& n : nodes_) {
    if (n->instance_id() == instance_id) return n.get();
  }
  return nullptr;
}

auto graph::find_source(std::string_view dst_node, std::string_view dst_slot) const
    -> std::optional<std::pair<std::string, std::string>> {
  for (auto const& e : edges_) {
    if (e.dst_node == dst_node && e.dst_slot == dst_slot) {
      return std::make_pair(e.src_node, e.src_slot);
    }
  }
  return std::nullopt;
}

}  // namespace cc::runtime
