#pragma once

// Plugin contract — the ONLY thing crossing the dlopen boundary.
// Pure C++, no Boost: plugins include just this header (and <cc/ir.hpp>).
// Host (cc-pipeit) loads it via Boost.DLL, fully hidden behind pimpl.

#include <cc/ir.hpp>

#include <string_view>

namespace cc {

// Bumped on any change to this contract; the host refuses mismatched plugins
// with a "rebuild against this cc-core" error.
inline constexpr int plugin_api_version = 1;

struct plugin_info {
  int api_version;
  const char* name;
  const char* kind;  // "frontend" | "backend"
};

class frontend {
 public:
  virtual ~frontend() = default;
  virtual bool compile(std::string_view source, ir::module& out) = 0;
};

class backend {
 public:
  virtual ~backend() = default;
  virtual bool emit(ir::module const& mod, std::string_view out_path) = 0;
};

}  // namespace cc

// Plugin entry points (extern "C", resolved by name via dlopen/Boost.DLL).
extern "C" cc::plugin_info cc_plugin_load();
extern "C" cc::frontend* cc_plugin_frontend();  // frontend plugins define this
extern "C" cc::backend* cc_plugin_backend();    // backend plugins define this
