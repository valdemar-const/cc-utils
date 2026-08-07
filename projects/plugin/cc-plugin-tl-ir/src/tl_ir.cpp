#include <cc/plugin.hpp>

#include <cc/astit.hpp>  // cc::ast::tl_program
#include <cc/astq.hpp>   // cc::astq::lower

namespace {

// Stage 2: type-erased AST -> neutral IR. Downcasts IAnyAst back to the
// concrete tl AST, then traverses it via cc-astq. The cast crosses the DSO
// boundary (tl_program's typeinfo is owned + exported by libcc-astit, which
// both tl plugins link), so it resolves correctly.
class tl_irgen final : public cc::ir_generator {
 public:
  bool generate(const cc::IAnyAst& ast, cc::ir::module& out) override {
    const auto* tl = dynamic_cast<const cc::ast::tl_program*>(&ast);
    if (tl == nullptr || tl->root == nullptr) return false;
    out = cc::astq::lower(*tl->root);
    return true;
  }
};

tl_irgen g_instance;

}  // namespace

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "tl-ir", "irgen"};
}

extern "C" cc::ir_generator* cc_plugin_irgen() {
  return &g_instance;
}
