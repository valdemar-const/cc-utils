#pragma once

#include "cc-astit_export.hpp"

namespace cc {

// Type-erased AST handle that crosses the plugin (dlopen) boundary.
//
// A frontend plugin returns a concrete language AST (e.g. cc::ast::tl_program)
// through this base; the matching ir_generator plugin dynamic_casts it back to
// the language type. Polymorphic + queried across DSOs, so the class carries
// default visibility (CC_ASTIT_API) and an out-of-line destructor (key function)
// so its typeinfo is owned + exported by libcc-astit and shared by all plugins.
class CC_ASTIT_API IAnyAst {
 public:
  virtual ~IAnyAst();
};

}  // namespace cc
