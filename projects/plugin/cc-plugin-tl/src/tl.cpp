#include <cc/plugin.hpp>

#include <cc/astit.hpp>
#include <cc/parseit.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace {

// Stage 1: source text -> type-erased tl AST (cc::ast::tl_program).
class tl_frontend final : public cc::frontend {
 public:
  std::unique_ptr<cc::IAnyAst> parse(std::string_view src) override {
    auto prog = cc::parseit::parse(src);
    if (!prog) return nullptr;
    auto carrier = std::make_unique<cc::ast::tl_program>();
    carrier->root = std::make_unique<cc::ast::program>(std::move(*prog));
    return carrier;  // upcast unique_ptr<tl_program> -> unique_ptr<IAnyAst>
  }
};

tl_frontend g_instance;

}  // namespace

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "tl", "frontend"};
}

extern "C" cc::frontend* cc_plugin_frontend() {
  return &g_instance;
}
