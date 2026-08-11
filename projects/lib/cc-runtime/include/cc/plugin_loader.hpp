#pragma once

#include "cc-runtime_export.hpp"
#include "cc/host.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cc::runtime {

// Loads cc-plugin-*.so files via dlopen, runs their cc_plugin_load (version
// check) + cc_plugin_register (populate the host registry). Holds the shared
// library handles alive for as long as the loader exists — outliving any node
// instances created from those plugins (their vtables/code live in the .so).
class CC_RUNTIME_API plugin_loader {
 public:
  plugin_loader();
  ~plugin_loader();

  plugin_loader(const plugin_loader&)            = delete;
  plugin_loader& operator=(const plugin_loader&) = delete;

  // Default search dirs (in priority order):
  //   1. $CCP_PLUGIN_PATH (colon-separated)
  //   2. directory of the running executable
  //   3. <exe dir>/../lib
  //   4. "."
  // Same convention as cc-pipeit.
  static auto default_search_dirs() -> std::vector<std::string>;

  // Open `cc-plugin-<name>.so` from any search dir, version-check, register.
  // Returns empty string on success; error description on failure.
  auto load(std::string_view name, host_registry& host) -> std::string;

  // Open a specific .so path. Used by load() and load_all().
  auto load_path(const std::string& path, host_registry& host) -> std::string;

  // Scan search dirs for `cc-plugin-*.so` and load all. Files that fail the
  // api_version check (or are otherwise malformed) are skipped with a stderr
  // note. Returns the number of successfully loaded plugins.
  auto load_all(host_registry& host) -> std::size_t;

 private:
  struct impl;
  std::unique_ptr<impl> pimpl_;
};

}  // namespace cc::runtime
