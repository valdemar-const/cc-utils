#pragma once

#include "cc-runtime_export.hpp"
#include "cc/any_value.hpp"
#include "cc/graph.hpp"
#include "cc/node.hpp"

#include <expected>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cc::runtime {

// Pull-based graph evaluator with simple per-run caching.
//
// Construct with a graph; call `pull(node_id, slot_id)` to evaluate one
// output slot. The runner walks upstream, calling `node::activate` on each
// node in dependency order, threading any_value through input/output pairs.
//
// MVP notes:
//   - no cycle detection beyond "currently computing" → reports first cycle.
//   - cache is per-runner; destroy the runner to drop all cached values,
//     or build a new runner after mutating the graph.
//   - returned pointers are stable until the runner is destroyed.
class CC_RUNTIME_API runner {
 public:
  using log_callback = std::function<void(std::string_view)>;

  // `pipeline_dir` is forwarded to every activate() call via the
  // activate_context, so nodes with path-typed properties can resolve
  // relative entries against the .pipeline file's directory. Leave empty
  // for unit tests or in-memory graphs that don't have a file on disk.
  explicit runner(graph& g, log_callback logger = {},
                  std::string pipeline_dir = {});

  // Pull value at (node_id, slot_id). Recursively activates upstream nodes.
  // Returns a pointer into the runner's cache (stable for the runner's
  // lifetime) or a failure describing the first problem encountered.
  auto pull(std::string_view node_id, std::string_view slot_id)
      -> std::expected<const any_value*, failure>;

 private:
  // Ensure all output slots of `node_id` are computed and cached. Returns
  // failure on the first upstream error.
  auto ensure_outputs(std::string_view node_id) -> std::expected<void, failure>;

  graph&             g_;
  log_callback       logger_;
  activate_context   ctx_;
  std::unordered_map<std::string, std::unordered_map<std::string, any_value>> cache_;
  std::unordered_set<std::string> completed_;
  std::unordered_set<std::string> in_progress_;
};

}  // namespace cc::runtime
