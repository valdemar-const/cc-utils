#pragma once

// Plugin contract — the ONLY thing crossing the dlopen boundary.
// Pure C++, no Boost: plugins include just this header (and cc/ir.hpp,
// cc/any_ast.hpp via cc-astit). Host (cc-pipeit) loads it via Boost.DLL,
// fully hidden behind pimpl.

#include <cc/any_ast.hpp>  // cc::IAnyAst (from cc-astit, exported typeinfo)
#include <cc/ir.hpp>

#include <memory>
#include <string_view>

namespace cc {

// Bumped on any change to this contract; the host refuses mismatched plugins
// with a "rebuild against this cc-core" error.
// v2: AST now crosses the boundary as a type-erased cc::IAnyAst; the frontend
//     produces it, a new ir_generator stage consumes it. (v1: source->ir.)
inline constexpr int plugin_api_version = 2;

struct plugin_info {
  int api_version;
  const char* name;
  const char* kind;  // "frontend" | "irgen" | "backend"
};

// Stage 1: source text -> type-erased language AST.
class frontend {
 public:
  virtual ~frontend() = default;
  virtual std::unique_ptr<IAnyAst> parse(std::string_view source) = 0;
};

// Stage 2: language AST -> neutral IR. Paired by language with a frontend;
// downcasts the IAnyAst back to the concrete language AST.
class ir_generator {
 public:
  virtual ~ir_generator() = default;
  virtual bool generate(const IAnyAst& ast, ir::module& out) = 0;
};

// Stage 3: IR -> executable. Language-agnostic; here the link to the frontend
// is lost — IR is the narrow waist.
class backend {
 public:
  virtual ~backend() = default;
  virtual bool emit(const ir::module& mod, std::string_view out_path) = 0;
};

}  // namespace cc

// Plugin entry points (extern "C", resolved by name via dlopen/Boost.DLL).
// A plugin .so defines the one matching its role.
extern "C" cc::plugin_info cc_plugin_load();
extern "C" cc::frontend* cc_plugin_frontend();        // frontend plugins
extern "C" cc::ir_generator* cc_plugin_irgen();       // ir_generator plugins
extern "C" cc::backend* cc_plugin_backend();          // backend plugins
