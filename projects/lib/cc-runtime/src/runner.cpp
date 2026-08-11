#include "cc/runner.hpp"

#include <algorithm>
#include <memory>       // std::addressof (any_with overloads operator&)
#include <utility>
#include <vector>

namespace cc::runtime {

runner::runner(graph& g) : g_{g} {}

auto runner::pull(std::string_view node_id, std::string_view slot_id)
    -> std::expected<const any_value*, failure> {
  // Identify the slot. Input slots are resolved via the upstream edge;
  // output slots trigger node activation (with caching).
  node* n = g_.find_node(node_id);
  if (!n) {
    return std::unexpected(failure{"unknown node " + std::string{node_id}});
  }
  const slot* target = nullptr;
  for (auto* s : n->slots()) {
    if (s->id() == slot_id) { target = s; break; }
  }
  if (!target) {
    return std::unexpected(failure{"node " + std::string{node_id} +
                                   " has no slot '" + std::string{slot_id} + "'"});
  }

  if (target->dir() == slot_dir::in) {
    // Input slot: pull from the upstream source output.
    auto src = g_.find_source(node_id, slot_id);
    if (!src) {
      return std::unexpected(failure{"unconnected input '" + std::string{slot_id} +
                                     "' on node " + std::string{node_id}});
    }
    return pull(src->first, src->second);
  }

  // Output slot: ensure node has computed outputs, return cached value.
  if (auto err = ensure_outputs(node_id); !err) {
    return std::unexpected(err.error());
  }
  auto outer = cache_.find(std::string{node_id});
  if (outer == cache_.end()) {
    return std::unexpected(failure{"no outputs cached for node " + std::string{node_id}});
  }
  auto inner = outer->second.find(std::string{slot_id});
  if (inner == outer->second.end()) {
    return std::unexpected(failure{"node " + std::string{node_id} +
                                   " has no output slot '" + std::string{slot_id} + "'"});
  }
  return std::addressof(inner->second);
}

auto runner::ensure_outputs(std::string_view node_id) -> std::expected<void, failure> {
  std::string key{node_id};
  if (completed_.count(key)) return {};
  if (in_progress_.count(key)) {
    return std::unexpected(failure{"cycle detected at node " + key});
  }

  node* n = g_.find_node(node_id);
  if (!n) {
    return std::unexpected(failure{"unknown node " + key});
  }
  in_progress_.insert(key);

  // 1. Resolve each declared input slot via upstream pull.
  std::vector<input_pair> inputs;
  for (auto* s : n->slots()) {
    if (s->dir() != slot_dir::in) continue;
    auto src = g_.find_source(node_id, s->id());
    if (!src) {
      in_progress_.erase(key);
      return std::unexpected(failure{"unconnected input '" + std::string{s->id()} +
                                     "' on node " + key});
    }
    auto result = pull(src->first, src->second);
    if (!result) {
      in_progress_.erase(key);
      return std::unexpected(result.error());
    }
    inputs.emplace_back(s->id(), *result);
  }

  // 2. Prepare output slots — get stable addresses inside cache_ for activate
  //    to write into. References to existing unordered_map elements remain
  //    valid across later inserts (only iterators would invalidate).
  auto& node_cache = cache_[key];
  std::vector<output_pair> outputs;
  for (auto* s : n->slots()) {
    if (s->dir() != slot_dir::out) continue;
    auto [it, _inserted] = node_cache.try_emplace(std::string{s->id()});
    outputs.emplace_back(s->id(), std::addressof(it->second));
  }

  // 3. Activate.
  auto result = n->activate(inputs, outputs);
  if (!result) {
    in_progress_.erase(key);
    cache_.erase(key);
    return std::unexpected(result.error());
  }

  in_progress_.erase(key);
  completed_.insert(key);
  return {};
}

}  // namespace cc::runtime
